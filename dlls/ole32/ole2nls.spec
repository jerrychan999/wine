name	ole2nls
type	win16

1 pascal GetUserDefaultLCID()      GetUserDefaultLCID
2 pascal GetSystemDefaultLCID()    GetSystemDefaultLCID
3 pascal GetUserDefaultLangID()    GetUserDefaultLangID
4 pascal GetSystemDefaultLangID()  GetSystemDefaultLangID
5 pascal GetLocaleInfoA(long long ptr word) GetLocaleInfo16
6 stub LCMAPSTRINGA
7 pascal16 GetStringTypeA(long long str word ptr) GetStringType16
8 pascal16 CompareStringA(long long str word str word) CompareString16
9 pascal16 RegisterNLSInfoChanged(ptr) RegisterNLSInfoChanged16
#10 stub WEP
11 stub LIBMAIN
12 stub NOTIFYWINDOWPROC
