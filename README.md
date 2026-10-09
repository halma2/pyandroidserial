# pyandroidserial
This is a pyserial implementation for android deployment. 
With this python applications can communicate with USB (UART) devices through USB OTG cable.
Originally this project was made for slcan (python-can), where the serial class of the slcan was rewritten with this class.
It means, this has not been tested in every situations yet.
The python version was 3.11 and the native c++ library used Qt Android 6.11 library.
The project was deployed to android with kivy/buildozer, where the compiled c++ directory "pyandroidserial.so" was added to the argument "app.android.add_libs_arm64_v8a".
