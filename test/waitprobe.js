// stage-J debug v4: probe wait semantics. One ares.wait(300), then report.
ares.setHomebrew(true);
ares.setRenderer("none");
ares.loadRom(ares.args[0]);
ares.resume();

ares.wait(300);
console.log("after wait(300)");
ares.waitVI(300);
console.log("after waitVI(300)");
