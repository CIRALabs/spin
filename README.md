
This is a prototype of the SPIN platform.

# What is SPIN?

SPIN stands for Security and Privacy for In-home Networks, it is a
traffic visualization tool (and in the future analyzer) intended to
help protect the home network with an eye on the Internet of Things
devices and the security problems they might bring.

Currently, the SPIN prototype is implemented as a package that can be
run on either a Linux system or an OpenWRT-based router; it can show
network activity in a graphical interface, and has the option to block
traffic on top of existing firewall functionality.

For a screenshot, see [here](/doc/images/prototype-20170103.png?raw=true).


# Building the source code

The SPIN prototype is tested on OpenWRT, Debian and Raspbian systems.

It also comes bundled with the Valibox router image software, available
as pre-built images for GL-Inet AR-150, VirtualBox, and the Raspberry
Pi 3. See the [Valibox website](https://valibox.sidnlabs.nl)

## On (Linux) PC

### Dependencies

- gcc
- make
- autoconf
- libnfnetlink-dev
- libnetfilter-conntrack-dev
- libnetfilter-queue-dev
- libnetfilter-log-dev
- libldns-dev

(TODO is this still needed?) - linux-headers-&lt;version&gt;

    `apt-get install gcc make autoconf libnfnetlink-dev libmnl-dev libnetfilter-queue-dev`

Library dependencies:

- libnfnetlink0
- libmnl

    `apt-get install libnfnetlink0 libmnl0`

Lua dependencies (for client tooling, and web API):

- libmosquitto-dev
- lua 5.1 (and luarocks for easy install of libs below)
- lua-mosquitto
- lua-minittp
- lua-websockets
- luabitop
- luaposix

    `apt-get install libmosquitto-dev`
    `luarocks install lua-mosquitto luabitop luaposix lua-minittp`

Runtime dependencies:
- mosquitto (or any MQTT software that supports websockets as well)
- kernel modules for conntrack and netfilter

### Building

Run in the source dir:

```
    autoreconf --install
    mkdir build
    (cd build; ../configure && make)
```


### Running from source tree

To run SPIN, you need to run two daemons; spind to collect data, and spin_webui for the traffic monitor to talk to. You'll also need an MQTT server, such as mosquitto, wich websockets support on port 1884 (as well as plain MQTT on port 1883).

The SPIN system is most useful when run on a gateway; there are several instructions on the web on how to set up a Debian system as a gateway. One example is [https://gridscale.io/en/community/tutorials/debian-router-gateway/](https://gridscale.io/en/community/tutorials/debian-router-gateway/).

To run spind from the source tree, with stdout output and debug logging, use:
    `(sudo) (cd ./src/build/spind/; spind -o -d)`

To run the webserver, use:
    `(sudo) (cd ./src/web_ui/; minittp-server -a 127.0.0.1 -p 8080 ./spin_webui.lua`


### System

After this step is complete, you can find the spin daemon in the spind directory. The tools/spin_print and tools/spin_config tools are supporting tools for older versions and deprectaed in the latest release.

SPIN sends its data to MQTT, which any MQTT client can then read. A web-based client ('the bubble app') can be found in `src/web_ui/static/spin_api/`, the main HTML file is `graph.html`, and depending on where you host it, you can access it from a browser with the URL `file://<path>/graph.html?mqtt_host=<ip address of MQTT server>`.

Commands and other data requests are sent to spin through a Web API, which is run using lua-minittp. All RPC calls are exposed through this as well.

## For OpenWRT

If you have a build environment for OpenWRT (see https://wiki.openwrt.org/doc/howto/build), you can add the following feed to the feeds.conf file:
src-git sidn https://github.com/SIDN/sidn_openwrt_pkgs

After running scripts/feeds update and scripts/feeds install, you can select the spin package in menuconfig under Network->SIDN->spin.

Running make package/spin/compile and make package/spin install will result in a spin-<version>.ipk file in bin/<architecture>.

This package has an extra file in addition to the ones described in the
previous section: a startup script /etc/init.d/spin; this script will
load the kernel module and start the spin_mqtt.lua daemon.


# Running SPIN

(NOTE: in the current version, the IP address of the router/server is
hardcoded to be 192.168.8.1; this is also the address used in this
example. We are working on deriving this address automatically, or at
the very least make it a configuration option).

When the OpenWRT package is installed, SPIN should start automatically
after a reboot. Simply use a browser to go to
http://192.168.8.1/www/spin/graph.html to see it in action.

When installed locally, a few manual steps are required:

(0. Configure your system to be a gateway, example instructions: [https://gridscale.io/en/community/tutorials/debian-router-gateway/](https://gridscale.io/en/community/tutorials/debian-router-gateway/))
1. Configure and start an MQTT service; this needs to listen to port 1883 (mqtt) and 1884 (websockets protocol).
2. Load the relevant kernel modules: `modprobe nf_conntrack_ipv4 nf_conntrack_ipv6 nfnetlink_log nfnetlink_queue`
3. Enable conntrack accounting: `sysctl net.netfilter.nf_conntrack_acct=1`
4. Start the spin daemon `(sudo) (cd ./src/build/spind/; spind -o -d)`
5. Start the spin Web daemon `(sudo) (cd ./src/web_ui/; minittp-server -a 127.0.0.1 -p 8080 ./spin_webui.lua`
5. Load the spin bubble app by visiting `http://127.0.0.1:8080/spin_graph/graph.html?mqtt_host=127.0.0.1`


# High-level technical overview

The software contains three parts:

- a daemon that aggregates traffic and DNS information (with nf_conntract and nflog) and sends it to MQTT
- a html/javascript front-end for the user
- a RESTful API (currently only a subset of the intended funcitonality)

The information that is sent to MQTT contains the following types:
* Traffic: information about traffic itself, source address, destination address, ports, and payload sizes
* Blocked: information about traffic that was blocked (by SPIN, not by the general firewall)
* DNS: domain names that have been resolved into IP addresses (so the visualizer can show which domain names were used to initiate traffic)

The daemon will also listen for configuration commands in the MQTT topic SPIN/commands. This will be replaces by RPC calls in the near future.


# Data protocols and APIs

## MQTT messages

For the channels and mqtt message formats, see [doc/mqtt_protocol.md](doc/mqtt_protocol.md)

## Web API

For the Web API description, see [doc/web_api.md](doc/web_api.md)

