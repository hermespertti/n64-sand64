// 60fps-capable smooth capture: one shot per VI frame
ares.setHomebrew(true);
ares.setRenderer("none");
ares.loadRom(ares.args[0]);
ares.resume();
var ok = ares.waitLog("[probe] f=60", 150);
console.log("start -> " + ok);
var N = 400;
for (var i = 0; i < N; i++) {
  ares.waitFrames(1);
  try { ares.screenshot().save("shots/sm_" + (i < 10 ? "00" : i < 100 ? "0" : "") + i + ".png"); }
  catch (e) { console.log("err " + i + " " + e); }
}
console.log("grab done");
