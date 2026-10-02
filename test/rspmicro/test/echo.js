ares.setHomebrew(true);
ares.setRenderer("none");
ares.loadRom(ares.args[0]);
ares.resume();
var ok = ares.waitLog("[mic] done", 120);
console.log("waitLog -> " + ok);
ares.systemPower(false);
