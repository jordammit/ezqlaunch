# ezQLaunch
Copyright © 2026 iKM Media (Jordan Siegler)  
  
ezQLaunch is a server browser/launcher for QuakeWorld, designed as both a  
standalone application and a background/service worker for [ezQWTF (ezQuakeWorld](https://github.com/jordammit/qlaunch)  
[Team Fortress)](https://github.com/jordammit/qlaunch).  
  
Forked from QLaunch by Cory Nelson.  
Developed for the QuakeWorld Team Fortress Unification Project.  
  
Unlike QLaunch, this fork does NOT use GameStat as it's backend, instead opting for  
a completely custom solution to query a master server, ping and obtain server/player  
info.  
  
When used in standalone mode by launching the executable directly (ezqlaunch.exe),  
it functions nearly the same as QLaunch, just with added QOL features.  
When used in service mode by launching the ezQWTF executable (ezqwtfcl.exe), it  
functions via an IPC pipe to funnel all information over to the client directly -  
essentially allowing the QW client to act as a frontend and ezQLaunch as a backend.  
  
## How to use
**As Standalone:**  
- Set a Master Server and Client using the "Edit Master" and "Set Client" buttons,  
then click Full Update.  
  
- Server lists are cached between sessions, allowing for a simple refresh of the   
cached list on relaunch.  
  
- Servers can be filtered according to user preference. Since this is developed to  
be used in conjunction with ezQWTF, the "TF Only" filter is enabled by default.  
  
**As Background Service:**  
All above options are available via the "Find Servers" menu within ezQWTF.  
Configuration is identical, with no additional options or requirements necessary.  
  
**Alternatively:**  
You can edit the variables in ezqlaunch.conf to your liking. Though created  
automatically via normal use, you can also create `favorites.txt` `neverscan.txt` and  
`history.txt` within the same directory as ezqlaunch.exe to manually add servers to  
the Favorites and Never Ping tabs, respectively. IP ranges added to the latter txt  
file also affect the Last Played column in all tabs. All IPs are separated by new  
line. The format is as follows:  
  
history.txt:  
```
ipaddr1:port	yyyy-mm-dd hh:mm
ipaddr2:port	yyyy-mm-dd hh:mm
```
  
favorites.txt, neverscan.txt:  
```
ipaddr1:port
ipaddr2:port
```
  
## Source Code
Just as QLaunch before it, the ezQLaunch source is available freely and licensed under  
an  MIT License.  Please make sure you understand it well before messing with the  
included source.  Though not required, I would love to hear from you if you use it.  
  
## Links
QLaunch Homepage - https://web.archive.org/web/20041205094956/http://dev.int64.org/qlaunch.html  
ezQWTF - https://github.com/jordammit/ezqwtf-source/  
