#ifndef WTSN_UI_PAGES_H
#define WTSN_UI_PAGES_H

#include "app/app.h"
#include "mvc/view.h"

wtsn_view *wtsn_page_dashboard_create(wtsn_app *app);
wtsn_view *wtsn_page_devices_create(wtsn_app *app);
wtsn_view *wtsn_page_tsn_create(wtsn_app *app);
wtsn_view *wtsn_page_tas_create(wtsn_app *app);
wtsn_view *wtsn_page_vlan_create(wtsn_app *app);
wtsn_view *wtsn_page_timesync_create(wtsn_app *app);
wtsn_view *wtsn_page_sensors_create(wtsn_app *app);
wtsn_view *wtsn_page_opcua_create(wtsn_app *app);
wtsn_view *wtsn_page_mqtt_create(wtsn_app *app);
wtsn_view *wtsn_page_trace_create(wtsn_app *app);
wtsn_view *wtsn_page_settings_create(wtsn_app *app);

#endif
