ares.setHomebrew(true);
ares.setRenderer("none");
ares.loadRom(ares.args[0]);
ares.resume();
ares.waitFrames(120);
var p = ares.controller(1);
p.hold("A");
ares.waitFrames(60);
p.release("A");
ares.waitFrames(30);
try { ares.screenshot().save("shots/play.png"); console.log("shot ok"); } catch (e) { console.log("err " + e); }
