This folder contains the recipes.cfg file which is needed for our
integration with LUCI. Unfortunately, the infra/config prefix for
this file is hardcoded into LUCI infrastructure so we cannot move
this to infra/luci folder where the other LUCI code lives.