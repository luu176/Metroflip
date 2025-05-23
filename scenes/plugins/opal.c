/*
 * opal.c - Parser for Opal card (Sydney, Australia).
 *
 * Copyright 2023 Michael Farrell <micolous+git@gmail.com>
 *
 * This will only read "standard" MIFARE DESFire-based Opal cards. Free travel
 * cards (including School Opal cards, veteran, vision-impaired persons and
 * TfNSW employees' cards) and single-trip tickets are MIFARE Ultralight C
 * cards and not supported.
 *
 * Reference: https://github.com/metrodroid/metrodroid/wiki/Opal
 *
 * Note: The card values are all little-endian (like Flipper), but the above
 * reference was originally written based on Java APIs, which are big-endian.
 * This implementation presumes a little-endian system.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "../../metroflip_i.h"
#include <flipper_application.h>

#include <lib/nfc/protocols/mf_desfire/mf_desfire.h>
#include <lib/nfc/protocols/mf_desfire/mf_desfire_poller.h>
#include "../../api/metroflip/metroflip_api.h"
#include "../../metroflip_plugins.h"

#include <applications/services/locale/locale.h>
#include <datetime.h>

#define TAG "Metroflip:Scene:Opal"

static const MfDesfireApplicationId opal_app_id = {.data = {0x31, 0x45, 0x53}};

static const MfDesfireFileId opal_file_id = 0x07;

static const char* opal_modes[5] =
    {"Rail / Metro", "Ferry / Light Rail", "Bus", "Unknown mode", "Manly Ferry"};

static const char* opal_usages[14] = {
    "New / Unused",
    "Tap on: new journey",
    "Tap on: transfer from same mode",
    "Tap on: transfer from other mode",
    NULL, // Manly Ferry: new journey
    NULL, // Manly Ferry: transfer from ferry
    NULL, // Manly Ferry: transfer from other
    "Tap off: distance fare",
    "Tap off: flat fare",
    "Automated tap off: failed to tap off",
    "Tap off: end of trip without start",
    "Tap off: reversal",
    "Tap on: rejected",
    "Unknown usage",
};

// Opal file 0x7 structure. Assumes a little-endian CPU.
typedef struct FURI_PACKED {
    uint32_t serial         : 32;
    uint8_t check_digit     : 4;
    bool blocked            : 1;
    uint16_t txn_number     : 16;
    int32_t balance         : 21;
    uint16_t days           : 15;
    uint16_t minutes        : 11;
    uint8_t mode            : 3;
    uint16_t usage          : 4;
    bool auto_topup         : 1;
    uint8_t weekly_journeys : 4;
    uint16_t checksum       : 16;
} OpalFile;

static_assert(sizeof(OpalFile) == 16, "OpalFile");

// Converts an Opal timestamp to DateTime.
//
// Opal measures days since 1980-01-01 and minutes since midnight, and presumes
// all days are 1440 minutes.
static void opal_days_minutes_to_datetime(uint16_t days, uint16_t minutes, DateTime* out) {
    out->year = 1980;
    out->month = 1;
    // 1980-01-01 is a Tuesday
    out->weekday = ((days + 1) % 7) + 1;
    out->hour = minutes / 60;
    out->minute = minutes % 60;
    out->second = 0;

    // What year is it?
    for(;;) {
        const uint16_t num_days_in_year = datetime_get_days_per_year(out->year);
        if(days < num_days_in_year) break;
        days -= num_days_in_year;
        out->year++;
    }

    // 1-index the day of the year
    days++;

    for(;;) {
        // What month is it?
        const bool is_leap = datetime_is_leap_year(out->year);
        const uint8_t num_days_in_month = datetime_get_days_per_month(is_leap, out->month);
        if(days <= num_days_in_month) break;
        days -= num_days_in_month;
        out->month++;
    }

    out->day = days;
}
bool opal_parse(const MfDesfireData* data, FuriString* parsed_data) {
    furi_assert(parsed_data);

    bool parsed = false;

    do {
        const MfDesfireApplication* app = mf_desfire_get_application(data, &opal_app_id);
        if(app == NULL) break;

        const MfDesfireFileSettings* file_settings =
            mf_desfire_get_file_settings(app, &opal_file_id);
        if(file_settings == NULL || file_settings->type != MfDesfireFileTypeStandard ||
           file_settings->data.size != sizeof(OpalFile))
            break;

        const MfDesfireFileData* file_data = mf_desfire_get_file_data(app, &opal_file_id);
        if(file_data == NULL) break;

        const OpalFile* opal_file = simple_array_cget_data(file_data->data);

        const uint8_t serial2 = opal_file->serial / 10000000;
        const uint16_t serial3 = (opal_file->serial / 1000) % 10000;
        const uint16_t serial4 = (opal_file->serial % 1000);

        if(opal_file->check_digit > 9) break;

        // Negative balance. Make this a positive value again and record the
        // sign separately, because then we can handle balances of -99..-1
        // cents, as the "dollars" division below would result in a positive
        // zero value.
        const bool is_negative_balance = (opal_file->balance < 0);
        const char* sign = is_negative_balance ? "-" : "";
        const int32_t balance = is_negative_balance ? labs(opal_file->balance) : //-V1081
                                                      opal_file->balance;
        const uint8_t balance_cents = balance % 100;
        const int32_t balance_dollars = balance / 100;

        DateTime timestamp;
        opal_days_minutes_to_datetime(opal_file->days, opal_file->minutes, &timestamp);

        // Usages 4..6 associated with the Manly Ferry, which correspond to
        // usages 1..3 for other modes.
        const bool is_manly_ferry = (opal_file->usage >= 4) && (opal_file->usage <= 6);

        // 3..7 are "reserved", but we use 4 to indicate the Manly Ferry.
        const uint8_t mode = is_manly_ferry ? 4 : opal_file->mode;
        const uint8_t usage = is_manly_ferry ? opal_file->usage - 3 : opal_file->usage;

        const char* mode_str = opal_modes[mode > 4 ? 3 : mode];
        const char* usage_str = opal_usages[usage > 12 ? 13 : usage];

        furi_string_printf(
            parsed_data,
            "\e#Opal: $%s%ld.%02hu\nNo.: 3085 22%02hhu %04hu %03hu%01hhu\n%s, %s\n",
            sign,
            balance_dollars,
            balance_cents,
            serial2,
            serial3,
            serial4,
            opal_file->check_digit,
            mode_str,
            usage_str);

        FuriString* timestamp_str = furi_string_alloc();

        locale_format_date(timestamp_str, &timestamp, locale_get_date_format(), "-");
        furi_string_cat(parsed_data, timestamp_str);
        furi_string_cat(parsed_data, " at ");

        locale_format_time(timestamp_str, &timestamp, locale_get_time_format(), false);
        furi_string_cat(parsed_data, timestamp_str);

        furi_string_free(timestamp_str);

        furi_string_cat_printf(
            parsed_data,
            "\nWeekly journeys: %hhu, Txn #%hu\n",
            opal_file->weekly_journeys,
            opal_file->txn_number);

        if(opal_file->auto_topup) {
            furi_string_cat_str(parsed_data, "Auto-topup enabled\n");
        }

        if(opal_file->blocked) {
            furi_string_cat_str(parsed_data, "Card blocked\n");
        }

        parsed = true;
    } while(false);

    return parsed;
}

static NfcCommand opal_poller_callback(NfcGenericEvent event, void* context) {
    furi_assert(event.protocol == NfcProtocolMfDesfire);

    Metroflip* app = context;
    NfcCommand command = NfcCommandContinue;

    FuriString* parsed_data = furi_string_alloc();
    Widget* widget = app->widget;
    furi_string_reset(app->text_box_store);
    const MfDesfirePollerEvent* mf_desfire_event = event.event_data;
    if(mf_desfire_event->type == MfDesfirePollerEventTypeReadSuccess) {
        nfc_device_set_data(
            app->nfc_device, NfcProtocolMfDesfire, nfc_poller_get_data(app->poller));
        const MfDesfireData* data = nfc_device_get_data(app->nfc_device, NfcProtocolMfDesfire);
        if(!opal_parse(data, parsed_data)) {
            furi_string_reset(app->text_box_store);
            FURI_LOG_I(TAG, "Unknown card type");
            furi_string_printf(parsed_data, "\e#Unknown card\n");
        }
        widget_add_text_scroll_element(widget, 0, 0, 128, 64, furi_string_get_cstr(parsed_data));

        widget_add_button_element(
            widget, GuiButtonTypeRight, "Exit", metroflip_exit_widget_callback, app);
        widget_add_button_element(
            widget, GuiButtonTypeCenter, "Save", metroflip_save_widget_callback, app);

        furi_string_free(parsed_data);
        view_dispatcher_switch_to_view(app->view_dispatcher, MetroflipViewWidget);
        metroflip_app_blink_stop(app);
        command = NfcCommandStop;
    } else if(mf_desfire_event->type == MfDesfirePollerEventTypeReadFailed) {
        view_dispatcher_send_custom_event(app->view_dispatcher, MetroflipCustomEventPollerSuccess);
        command = NfcCommandContinue;
    }

    return command;
}

static void opal_on_enter(Metroflip* app) {
    dolphin_deed(DolphinDeedNfcRead);

    if(app->data_loaded) {
        Storage* storage = furi_record_open(RECORD_STORAGE);
        FlipperFormat* ff = flipper_format_file_alloc(storage);
        if(flipper_format_file_open_existing(ff, app->file_path)) {
            mf_desfire_load(app->mfdes_data, ff, 2);
            FuriString* parsed_data = furi_string_alloc();
            Widget* widget = app->widget;

            furi_string_reset(app->text_box_store);
            opal_parse(app->mfdes_data, parsed_data);
            widget_add_text_scroll_element(
                widget, 0, 0, 128, 64, furi_string_get_cstr(parsed_data));

            widget_add_button_element(
                widget, GuiButtonTypeRight, "Exit", metroflip_exit_widget_callback, app);
            widget_add_button_element(
                widget, GuiButtonTypeCenter, "Delete", metroflip_delete_widget_callback, app);
            furi_string_free(parsed_data);
            view_dispatcher_switch_to_view(app->view_dispatcher, MetroflipViewWidget);
        }
        flipper_format_free(ff);
    } else {
        // Setup view
        Popup* popup = app->popup;
        popup_set_header(popup, "Apply\n card to\nthe back", 68, 30, AlignLeft, AlignTop);
        popup_set_icon(popup, 0, 3, &I_RFIDDolphinReceive_97x61);

        // Start worker
        view_dispatcher_switch_to_view(app->view_dispatcher, MetroflipViewPopup);
        nfc_scanner_alloc(app->nfc);
        app->poller = nfc_poller_alloc(app->nfc, NfcProtocolMfDesfire);
        nfc_poller_start(app->poller, opal_poller_callback, app);

        metroflip_app_blink_start(app);
    }
}

static bool opal_on_event(Metroflip* app, SceneManagerEvent event) {
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == MetroflipCustomEventCardDetected) {
            Popup* popup = app->popup;
            popup_set_header(popup, "DON'T\nMOVE", 68, 30, AlignLeft, AlignTop);
            consumed = true;
        } else if(event.event == MetroflipCustomEventCardLost) {
            Popup* popup = app->popup;
            popup_set_header(popup, "Card \n lost", 68, 30, AlignLeft, AlignTop);
            consumed = true;
        } else if(event.event == MetroflipCustomEventWrongCard) {
            Popup* popup = app->popup;
            popup_set_header(popup, "WRONG \n CARD", 68, 30, AlignLeft, AlignTop);
            consumed = true;
        } else if(event.event == MetroflipCustomEventPollerFail) {
            Popup* popup = app->popup;
            popup_set_header(popup, "Failed", 68, 30, AlignLeft, AlignTop);
            consumed = true;
        }
    } else if(event.type == SceneManagerEventTypeBack) {
        scene_manager_search_and_switch_to_previous_scene(app->scene_manager, MetroflipSceneStart);
        scene_manager_set_scene_state(app->scene_manager, MetroflipSceneStart, MetroflipSceneAuto);
        consumed = true;
    }

    return consumed;
}

static void opal_on_exit(Metroflip* app) {
    widget_reset(app->widget);
    metroflip_app_blink_stop(app);
    if(app->poller && !app->data_loaded) {
        nfc_poller_stop(app->poller);
        nfc_poller_free(app->poller);
    }
}

/* Actual implementation of app<>plugin interface */
static const MetroflipPlugin opal_plugin = {
    .card_name = "Opal",
    .plugin_on_enter = opal_on_enter,
    .plugin_on_event = opal_on_event,
    .plugin_on_exit = opal_on_exit,

};

/* Plugin descriptor to comply with basic plugin specification */
static const FlipperAppPluginDescriptor opal_plugin_descriptor = {
    .appid = METROFLIP_SUPPORTED_CARD_PLUGIN_APP_ID,
    .ep_api_version = METROFLIP_SUPPORTED_CARD_PLUGIN_API_VERSION,
    .entry_point = &opal_plugin,
};

/* Plugin entry point - must return a pointer to const descriptor  */
const FlipperAppPluginDescriptor* opal_plugin_ep(void) {
    return &opal_plugin_descriptor;
}
