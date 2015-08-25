# bitalino-max

**bitalino** [Max](https://cycling74.com/products/max/) object for communication with the [BITalino](www.bitalino.com) BlueTooth device.   
Based on the BITalino cpp API by PLUX - Wireless Biosignals, S.A.   
The files bitalino.h and bitalino.cpp have been added here for convenience, the original ones can be found [there](https://github.com/BITalinoWorld/cpp-api).   
This object should be compiled with Max SDK version 6 or greater.   
Clone the repository in any subfolder of Max SDK examples folder and compile with XCode.

## messages to inlet

- start : starts a thread that will connect to BITalino through the API. only 1 instance of the object is allowed to connect to BITalino at the same time
- stop : stops the thread and releases connection with BITalino
- poll <ms_poll_interval> : starts polling frames
- nopoll : stops polling frames but keeps connection with BITalino alive

## messages from outlet

come in OSC-flavour, corresponding to each sensor channel of BITalino (/EMG, /EDA, /ECG, /ACCEL, /LUX, and the sixth channel which is unidentified at the time of this writing).
