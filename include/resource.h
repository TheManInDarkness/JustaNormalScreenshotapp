#pragma once

// ---------------------------------------------------------------- resources
#define IDI_APP_ICON                    101

#define IDR_TRAY_MENU                   200

#define IDR_ICON_CHECK                  300
#define IDR_ICON_GEAR                   301
#define IDR_ICON_CANCEL                 302
#define IDR_ICON_DELETE                 303
#define IDR_ICON_REDO                   304

// ------------------------------------------------------------------ dialogs
#define IDD_SETTINGS                    400
#define IDD_STITCH                      401
#define IDD_PDFCONVERT                  402

// -------------------------------------------------------------- tray/menu id
#define ID_TRAY_OPEN                    1000
#define ID_TRAY_CAPTURE                 1002
#define ID_TRAY_STITCH                  1003
#define ID_TRAY_PDF                     1004
#define ID_TRAY_SETTINGS                1005
#define ID_TRAY_EXIT                    1006

// Gear popup menu on the selection overlay
#define ID_OVERLAY_AUTOSCROLL           1100
#define ID_OVERLAY_MANUALSCROLL         1101
#define ID_OVERLAY_EXTRACTTEXT          1102

// ---------------------------------------------------------- settings dialog
#define IDC_TAB                         1200

// General tab
#define IDC_CHK_STARTUP                 1210
#define IDC_CHK_NOTIFICATIONS           1211
#define IDC_CHK_FOLLOW_THEME            1212
#define IDC_BTN_OPEN_LOGS               1213
#define IDC_CHK_START_MINIMIZED         1214

// Output tab
#define IDC_CHK_COPY                    1220
#define IDC_EDIT_FOLDER                 1223
#define IDC_BTN_BROWSE_FOLDER           1224
#define IDC_EDIT_PATTERN                1225
#define IDC_COMBO_FORMAT                1226
#define IDC_EDIT_QUALITY                1227
#define IDC_STATIC_QUALITY              1228

// Capture tab
#define IDC_CHK_CURSOR                  1230
#define IDC_EDIT_DELAY                  1232
#define IDC_EDIT_SETTLE                 1233

// Hotkeys tab
#define IDC_HOTKEY_CAPTURE              1240
#define IDC_HOTKEY_CAPTURE_ALT          1241
#define IDC_HOTKEY_SCROLLSHOT           1243
#define IDC_HOTKEY_STOP_AUTOSCROLL      1242
#define IDC_STATIC_HOTKEY_WARN          1244

// PDF tab.
//
// Drop-downs rather than radio groups: Win32 forms an auto-radio group from
// the *creation order* of the controls, which a two-column layout interleaves
// - the result was one group containing a single button that could never be
// unchecked, and another mixing page size with page layout. A combo box
// cannot express that state at all.
#define IDC_CHK_AUTO_PDF                1250
#define IDC_COMBO_PDF_LAYOUT            1251
#define IDC_COMBO_PDF_PAGESIZE          1252
#define IDC_COMBO_PDF_KEEP              1253
#define IDC_COMBO_PDF_DEST              1254
#define IDC_EDIT_PDF_FOLDER             1257
#define IDC_BTN_PDF_BROWSE              1258

// ------------------------------------------------------------ main window --
#define ID_GAL_CAPTURE                  1500
#define ID_GAL_STITCH                   1501
#define ID_GAL_PDF                      1502
#define ID_GAL_SETTINGS                 1503
#define ID_GAL_REFRESH                  1504
#define ID_GAL_FOLDER                   1505
#define ID_GAL_COPY                     1506
#define ID_GAL_OPEN                     1507
#define ID_GAL_DELETE                   1508
#define ID_GAL_LIST                     1509
#define ID_GAL_CANVAS                   1510
#define ID_GAL_STATUS                   1511
#define ID_GAL_EXTRACT                  1512

// ------------------------------------------------------------- stitch tool
#define IDC_IMAGE_LIST                  1300
#define IDC_PREVIEW_CANVAS              1301
#define IDC_RADIO_VERTICAL              1302
#define IDC_RADIO_HORIZONTAL            1303
#define IDC_CHECK_OVERLAP               1304
#define IDC_RADIO_LEFT                  1305
#define IDC_RADIO_CENTER                1306
#define IDC_RADIO_RIGHT                 1307
#define IDC_GAP_EDIT                    1308
#define IDC_STITCH_SAVE                 1309
#define IDC_COPY_RESULT                 1310
#define IDC_STITCH_ADD                  1311
#define IDC_STITCH_REMOVE               1312
#define IDC_STITCH_UP                   1313
#define IDC_STITCH_DOWN                 1314

// ---------------------------------------------------------- pdf convert tool
#define IDC_PDF_IMAGE_LIST              1400
#define IDC_RADIO_A4                    1401
#define IDC_RADIO_LETTER                1402
#define IDC_RADIO_PNG                   1403
#define IDC_RADIO_JPEG                  1404
#define IDC_RADIO_SLICE_PAGES           1405
#define IDC_RADIO_SLICE_LONG            1406
#define IDC_RADIO_SLICE_FIT             1412
#define IDC_SLICE_PREVIEW_CANVAS        1407
#define IDC_PDF_CONVERT                 1408
#define IDC_PDF_ADD                     1409
#define IDC_PDF_REMOVE                  1410
#define IDC_PDF_PAGECOUNT               1411

#ifndef IDC_STATIC
#define IDC_STATIC                      (-1)
#endif
