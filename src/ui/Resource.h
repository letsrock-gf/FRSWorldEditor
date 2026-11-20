#pragma once

#define IDR_MAIN_MENU          101
#define IDR_ACCELERATORS       102
#define IDI_APP                103
#define IDR_BLENDER_SCRIPT     104

// File
#define ID_FILE_OPEN_GAME      40001
#define ID_FILE_EXIT           40002
#define ID_FILE_BLENDER        40003

// World
#define ID_WORLD_LOAD_ALL      40101
#define ID_WORLD_FRAME         40102
#define ID_WORLD_CLOSE         40103
#define ID_WORLD_NEW           40104
#define ID_WORLD_OPEN          40105
#define ID_WORLD_SAVE          40106
#define ID_WORLD_SAVE_AS       40107

// View
#define ID_VIEW_TEXTURED       40201
#define ID_VIEW_UNLIT          40207
#define ID_VIEW_VERTEX_COLOR   40202
#define ID_VIEW_WIREFRAME      40203
#define ID_VIEW_FRUSTUM_CULL   40204
#define ID_VIEW_BACKFACE_CULL  40205
#define ID_VIEW_BACKDROP       40206
#define ID_VIEW_SCENERY        40208
#define ID_VIEW_LIBRARY        40210
#define ID_VIEW_ALTERNATE      40211
#define ID_VIEW_STREAMING      40213
#define ID_FILE_EXPORT         40120
#define ID_FILE_IMPORT         40121
#define ID_VIEW_COLLISION      40214

// MINIMAP PREVIEW (temporary)
#define ID_MINIMAP_GENERATE    40215
#define ID_MINIMAP_SHOW        40216
#define ID_MINIMAP_ORIGINALS   40217

// Help
#define ID_HELP_ABOUT          40301
#define ID_HELP_SUPPORT        40302
#define ID_HELP_README         40303

// Textures
#define ID_VIEW_TEXTURES       40209

#define IDD_TEXTURES           210
#define IDC_TEX_LIST           2101
#define IDC_TEX_PREVIEW        2102
#define IDC_TEX_INFO           2103
#define IDC_TEX_SAVE           2104
#define IDC_TEX_ZOOM           2105
#define IDC_TEX_REPLACE        2106
#define IDC_TEX_RESTORE        2107
#define IDC_TEX_SEARCH         2108

// Child controls
#define IDC_VIEWPORT           1001
#define IDC_REGION_COMBO       1002
#define IDC_SECTION_TREE       1003
#define IDC_STATUS_BAR         1004
#define IDC_SAVE_CAMERA        1005
#define IDC_GO_CAMERA          1006
#define IDC_LAUNCH_GAME        1007
#define IDC_TOOLBAR            1008
#define IDC_SIDE_TABS          1009
#define IDC_SIDE_SEARCH        1010
#define IDC_LIB_SHELF          1011
#define IDC_LIB_USED           1012
#define IDC_LIB_LIST           1013

// Edited meshes
#define ID_FILE_OVERLAYS       40122
#define IDD_OVERLAYS           220
#define IDC_OVERLAY_LIST       2201
#define IDC_OVERLAY_REMOVE     2202
#define IDC_OVERLAY_CLOSE      2203

// Placing scenery
#define ID_OBJECT_PLACE        40401
#define ID_OBJECT_PROPERTIES   40402
#define ID_OBJECT_DELETE       40403
#define ID_OBJECT_SAVE         40404
#define ID_OBJECT_GRAB         40405
#define ID_OBJECT_ROTATE       40406
#define ID_OBJECT_SNAP         40407
#define ID_OBJECT_COPY         40408
#define ID_OBJECT_PASTE        40409
#define ID_FILE_EDITS          40410
#define ID_OBJECT_BLENDER      40411
#define ID_OBJECT_NEW_BLENDER  40412
#define ID_OBJECT_NEW_OBJ      40413
#define ID_OBJECT_PLACE_MINE   40414
#define ID_OBJECT_DELETE_MINE  40415
#define ID_GIZMO_MOVE          40416
#define ID_GIZMO_ROTATE        40417
#define ID_GIZMO_SCALE         40418
#define ID_OBJECT_SELECT_SAME  40419
// Camera > Travel To: districts from here, zones from ID_TRAVEL_ZONE.
#define ID_TRAVEL_DISTRICT     46000
#define ID_TRAVEL_ZONE         46100
#define IDD_MY_OBJECTS         280
#define IDC_MINE_LIST          2901

// Sent to every editor extension after the setup changed the other games' folders
// (ui/SetupWizard.h, OtherGameFolder): their model libraries read them again.
#define ID_LIBRARY_FOLDERS_CHANGED 46900

// Commands from 47000 up are left to editor extensions (ui/EditorExtension.h).
#define ID_EXTENSION_FIRST     47000

#define IDD_PLACE_MODEL        230
#define IDC_PLACE_FILTER       2301
#define IDC_PLACE_LIST         2302
#define IDC_PLACE_INFO         2303
#define IDC_PLACE_USED         2304
#define IDC_PLACE_CATEGORY     2305

#define IDD_PLACEMENT          240
#define IDC_PL_X               2401
#define IDC_PL_Y               2402
#define IDC_PL_Z               2403
#define IDC_PL_HEADING         2404
#define IDC_PL_PITCH           2405
#define IDC_PL_ROLL            2406
#define IDC_PL_SCALE           2407
#define IDC_PL_INFO            2408
#define IDC_PL_SNAP            2409

#define IDD_EDITS              250
#define IDC_EDITS_LIST         2501
#define IDC_EDITS_INFO         2502
#define IDC_EDITS_REVERT       2503
#define IDC_EDITS_REFRESH      2504

// Properties panel
#define IDC_PROP_TITLE         2601
#define IDC_PROP_FACTS         2602
#define IDC_PROP_X             2603
#define IDC_PROP_Y             2604
#define IDC_PROP_Z             2605
#define IDC_PROP_HEADING       2606
#define IDC_PROP_PITCH         2607
#define IDC_PROP_ROLL          2608
#define IDC_PROP_SCALE         2609
#define IDC_PROP_SNAP          2610
#define IDC_PROP_REMOVE        2611
#define IDC_PROP_GRID          2612
#define IDC_PROP_CATEGORIZED   2613
#define IDC_PROP_ALPHABETICAL  2614
#define IDC_PROP_BLENDER       2615
#define IDC_PROP_COLLISION     2616

// Setup wizard
#define IDD_SETUP_FOLDER       260
#define IDD_SETUP_PARTS        261
#define IDD_SETUP_DONE         262
#define IDC_SETUP_PATH         2701
#define IDC_SETUP_BROWSE       2702
#define IDC_SETUP_FOLDER_STATUS 2703
#define IDC_SETUP_ASILOADER    2704
#define IDC_SETUP_ASILOADER_NOTE 2705
#define IDC_SETUP_LOADER       2706
#define IDC_SETUP_FASTBOOT     2707
#define IDC_SETUP_FASTBOOT_ALWAYS 2708
#define IDC_SETUP_DISABLE_OLD  2709
#define IDC_SETUP_PARTS_NOTE   2710
#define IDC_SETUP_RESULT       2711
#define IDC_SETUP_LINK         2712
#define IDC_SETUP_U1_PATH      2713
#define IDC_SETUP_U1_BROWSE    2714
#define IDC_SETUP_MW_PATH      2715
#define IDC_SETUP_MW_BROWSE    2716
#define IDC_SETUP_U1_STATUS    2717
#define IDC_SETUP_MW_STATUS    2718
#define IDC_SETUP_MAIN         2719
#define IDC_SETUP_NFSW_PATH    2720
#define IDC_SETUP_NFSW_BROWSE  2721
#define IDC_SETUP_NFSW_STATUS  2722
#define IDC_SETUP_LOADER_NOTE  2723
#define IDC_SETUP_FASTBOOT_NOTE 2724

// Worlds (sets of edits)
#define ID_EDIT_UNDO           40420
#define ID_EDIT_REDO           40421
#define ID_VIEW_CLIP           40422
#define ID_CAMERA_FRAME_SEL    40423
#define ID_VIEW_FRONT          40430
#define ID_VIEW_BACK           40431
#define ID_VIEW_RIGHT          40432
#define ID_VIEW_LEFT           40433
#define ID_VIEW_TOP            40434
#define ID_VIEW_BOTTOM         40435
#define ID_VIEW_OPPOSITE       40436
#define ID_VIEW_ORBIT_LEFT     40437
#define ID_VIEW_ORBIT_RIGHT    40438
#define ID_VIEW_ORBIT_UP       40439
#define ID_VIEW_ORBIT_DOWN     40440
#define IDC_CLIP_SENSITIVITY   2814
#define IDC_CLIP_ZOOM          2815
#define IDD_CLIP               290   // 272 is upstream's IDD_PUBLISH_WORLD since 0.2.1
#define IDC_CLIP_START         2811
#define IDC_CLIP_END           2812
#define IDC_CLIP_DETAIL        2813
#define IDD_NEW_WORLD          270
#define IDD_OPEN_WORLD         271
#define IDC_WORLD_NAME         2801
#define IDC_WORLD_LIST         2802
#define IDC_WORLD_NOTE         2803
#define IDD_PUBLISH_WORLD      272
#define IDC_PUBLISH_NOTE       2804
#define IDC_PUBLISH_NAME       2805
#define IDC_PUBLISH_AUTHOR     2806
#define IDC_PUBLISH_DESCRIPTION 2807
#define ID_WORLD_PUBLISH       40108
