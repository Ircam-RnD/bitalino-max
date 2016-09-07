# bitalino-max

**bitalino** [Max](https://cycling74.com/products/max/) object for communication with the [BITalino](www.bitalino.com) BlueTooth device.   
This object has been developed by the ISMM team at IRCAM, within the context of the RAPID-MIX project, funded by the European Union’s Horizon 2020 research and innovation programme.   
It is based on the BITalino cpp API by PLUX - Wireless Biosignals, S.A.   
This object should be compiled with Max SDK version 6 or greater.   

## messages to inlet

- connect : starts a thread that will connect to BITalino through the API. only 1 instance of the object is allowed to connect to BITalino at the same time.
- disconnect : stops the thread and releases the connection with BITalino.

## attributes

- continuous : if set to 0 (default value), all incoming frames from bluetooth are output immediately after they have been received. if set to 1, enables an internal clock reconstructing the signal at a constant rate determined by the interval attribute. this mode introduces a little latency because it uses an internal FIFO of 120 frames.
- interval : (default value 2) determines the interval in milliseconds between 2 consecutive frames when continuous mode is on. theoretical value should be 1 ms because the device's sampling rate is 1kHz, but due to inconsistency of the bluetooth transmission rate this can lead to duplicate frames if the FIFO gets empty.

## messages from outlet

come in OSC-flavour, corresponding to each analog sensor channel of BITalino : /A1, /A2, /A3, /A4, /A5 and /A6.

## notes

should now work with all versions of BITalino boards.   
only Mac version is stable for the moment, windows version is still a work in progress.
