Autodesk Scaleform 
Compilation Instructions for Android

Two sample players are included:
FxPlayerMobile - Flash player utilizing a standard Android Java framework with control panel for switching files,
                 displaying frame rate, and changing several runtime options.
FxPlayerTiny - Flash player utilizing an Android Native Activity self contained in a single source CPP file
               (Apps/Samples/GFxPlayerTiny/GFxPlayerTinyAndroid.cpp).

FxShippingPlayer is also included, which is a way of building apps with their Flash assets embedded into the .apk.
Please note that FxShippingPlayer enables FMOD audio by default.  You must obtain a license from Firelight Technologies Pty,
Ltd. to use this option.  FMOD support can be disabled by modifying the .def files located in LocalApps.

Instructions:

1) Build GFx, specifying the Android platform:
   $ make P=Android

2) As long as you are not modifying the Scaleform core libraries, you can now iterate by simply rebuilding the APKs 
   in one of the following ways:
   
   a) Command-line: By running 'ant debug' within one of the project directories found in Integrations/Android.
   
   b) GUI with Debugging: By loading one of the project directories found in Integrations/Android into Eclipse:
        1) File -> Import -> Android -> Existing Android Code Into Workspace
        2) Browse to one of the directories found in Integrations/Android within the SDK installation
        3) Right Click build.xml in Project Explorer -> Run As -> 1 Ant Build
        
3) For the Android x86 packages, when building with 2a or 2b above instead of make, initially you will need to manually update
   the following file within each project you want to use:

       in jni/Application.mk   
       change this:   
       SF_APP_ABI := armeabi-v7a
       to this:
       SF_APP_ABI := x86
      
Running Flash files:

1) Add the SWF(s) you want to play to to root of your sdcard (whichever one is mounted as /sdcard on the actual device).

2) To change the default startup file change the path that is used in FxPlayerMobile.cpp/GFxPlayerTinyAndroid.cpp:
   
   #define FXPLAYER_FILENAME  "flash.swf"
   
3) Alternatively you can use the Flash/SWF Intent by selecting a file in any supported File Browser.

4) If this is an eval Android Package, copy sf_license.txt to the location “/sdcard” on the device using whatever file explorer/manager
is available (note that it may default to that location and not allow access to the root of the file system).
   
Using FxPlayerMobile:

1) Extend the control panel by hitting the standard menu key on your Android device.
   
2) The buttons on the HUD are:
   Arrows                  - Change file (the initial file will be displayed out of rotation)
   Stopwatch               - Watch white: Show FPS; Arrows white: Fast forward
   Pause                   - Pauses or resumes the movie
   Status area:
     FPS                   - Frame rate if enabled
     Tri DP Mesh Mask      - Triangles, draw primitives, and masks
     Advance/Display       - Advance and display times
     Memory                - Memory allocated, used
   Rectangle with corners  - Toggle view clipping
   Two rectangles          - Batch profile mode
   Three rectangles        - Fill profiling mode
   Circle with number      - Curve tolerance (quality / performance tradeoff)
   Filled quarter circle   - Toggle EdgeAA

Using FxShippingPlayer:

Apps based on FxShippingPlayer are defined by a .def file in LocalApps. See one of the included def files for a description
of the available options. After building, each app has its own directory in LocalApps where its build files are stored. 
Each of these apps are built when you run make P=Android. You can modify the sources in LocalApps, but building may overwrite
them when run again. Save your changes in another location while working with these apps.

If you enable AUTORUN for an app, have it enabled for only one app at a time. AUTODEPLOY can be enabled for any number of apps.


Use of FxShippingPlayer has some limitations:

1. These apps will only work with the build configuration that was first used when building them. If you want
   to build a different configuration, delete the auto-generated Android directory from your app's directory in LocalApps.
2. There is no built-in support for icons. Manually place your icons in the appropriate place inside the generated Android
   directory for your app (typically <app-directory>/res/drawable-*).
3. The build process is not completely robust and various changes to the main GFx sources can cause the existing LocalApps
   directories to become unusable, or files that you have modified to be overwritten. If you make changes to FxShippingPlayer.cpp
   or other source files within LocalApps, please save your changes somewhere else while working on these apps.

   
Autodesk Scaleform
https://gameware.autodesk.com/scaleform/developer/
