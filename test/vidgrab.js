// capture one shot per probe line (probe = every 30 sim frames)
ares.setHomebrew(true);
ares.setRenderer("none");
ares.loadRom(ares.args[0]);
ares.resume();
var steps = [30,60,90,120,150,180,210,240,270,300,330,360,390,420,450,
             480,510,540,570,600,660,720,780,840,900,960,1020,1080,1140,
             1200,1260,1320,1380,1440,1500];
for (var i = 0; i < steps.length; i++) {
  var f = steps[i];
  var ok = ares.waitLog("[probe] f=" + f, 150);
  console.log("f=" + f + " -> " + ok);
  if (!ok) break;
  try {
    ares.waitFrames(3);
    ares.screenshot().save("shots/vid_" + (i < 10 ? "0" : "") + i + ".png");
  } catch (e) { console.log("err f" + f + " " + e); }
}
console.log("grab done");
