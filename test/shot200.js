// shot during SLOWHOLD window via log-driven wait:
// probe f=390 -> frame 400 soon begins its 10000-step hold (~50 s sim);
// the VI keeps scanning frame 399's finished render the whole time.
ares.setHomebrew(true);
ares.setRenderer("none");
ares.loadRom(ares.args[0]);
ares.resume();
var ok = ares.waitLog("[probe] f=390", 120);
console.log("waitLog -> " + ok);
try {
  ares.screenshot().save("shots/s1.png");
  console.log("shot ok");
} catch (e) { console.log("err " + e); }
