# ESP8266_WLAN_speaker
Using an ESP8266 to playback an audio stream over WiFi using 7-bit (8-bit dithered) PWM.

To playback a mp3 file, simply call e.g.:

avconv -i gong.mp3 -f s32be -acodec pcm_u8 -ac 1 -ar 33000 tcp://192.168.1.100:5522

Youtube demo video:
https://www.youtube.com/watch?v=Ai2RrCrgZ1c

Schematics can be found in this thread in the FHEM forum (german language):
https://forum.fhem.de/index.php?topic=71087.0
