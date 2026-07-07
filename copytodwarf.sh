cd build-dwarf
tar -cz audiotomidi.lv2/ | ssh root@192.168.51.1 "tar -xz -C /root/.lv2/"
