savedcmd_rkopnu.mod := printf '%s\n'   rocket_core.o rocket_device.o rocket_drv.o rocket_gem.o rocket_job.o | awk '!x[$$0]++ { print("./"$$0) }' > rkopnu.mod
