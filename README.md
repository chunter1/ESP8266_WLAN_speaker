# ESP8266_WLAN_speaker
Using an ESP8266 to playback an audio stream over WiFi using 7-bit (8-bit dithered) PWM.

To playback a mp3 file, simply call e.g.:

avconv -i gong.mp3 -f s32be -acodec pcm_u8 -ac 1 -ar 33000 tcp://192.168.1.100:5522

Where the IP is the IP of your esp8266 and gong.mp3 the path to the music file.

Youtube demo video:
https://www.youtube.com/watch?v=Ai2RrCrgZ1c

Schematics can be found in this thread in the FHEM forum (german language):
https://forum.fhem.de/index.php?topic=71087.0

At the moment there are several options for the amplification:
* NPN-Transistor like BC107 (see here: https://forum.fhem.de/index.php?action=dlattach;topic=71087.0;attach=78026)
* [PAM8302A (click for circuit diagram)](Documentation/CircuitDiagramWithPAM8302A.png)
* active computer-speaker

## How to on Windows

avconv is a cmd tool available for linux and Windows. If your on Windows go to the Libav site and download the latest windows build:
http://builds.libav.org/windows/nightly-gpl/ (avconv is included there).

To "Install" you just have to unpack that archive somewhere. Now Navigate your command prompt into that unpacked folder and further down into
"usr" -> "bin". Now you can use the command from the section above to play your file:

avconv -i gong.mp3 -f s32be -acodec pcm_u8 -ac 1 -ar 33000 tcp://192.168.1.100:5522

Where the IP is the IP of your esp8266 and gong.mp3 the path to the music file.
