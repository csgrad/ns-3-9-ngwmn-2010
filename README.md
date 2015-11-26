# ns-3-9-ngwmn
Modifications to ns-3.9 used for the publication of the NGWMN/BWCCA 2010 paper "Adaptive Mixed Bias Resource Allocation for Wireless Mesh Networks"

Since it has been some time since this code has been compiled - there are few "tweaks" to get it to run correctly in modern Ubuntu (15.10) with the current g++ (4.9.2).

See this link: https://www.nsnam.org/wiki/HOWTO_build_old_versions_of_ns-3_on_newer_compilers

The vanilla unmodified version of ns-3.9 can be downloaded here: https://www.nsnam.org/ns-3-9/

So far I require this to build the simulator: CXXFLAGS="-Wall" ./waf configure --disable-python

In addition, to get it compile I required the following changes:

* In src/core/unix-system-wall-clock-ms.cc and src/helper/animation-interface.cc
  * ```#include <unistd.h>```
* In src/common/spectrum-model.h
  * ```#include <cstddef>```
* In src/devices/emu/emu-net-device.cc
  * ```#include <sys/types.h>```
  * ```#include <unistd.h>```
* In src/devices/tap-bridge/tap-bridge.cc
  * ```#include <sys/types.h>```
  * ```#include <unistd.h>```
