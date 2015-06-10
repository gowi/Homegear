# GNU Make solution makefile autogenerated by Premake
# Type "make help" for usage help

ifndef config
  config=release
endif
export config

PROJECTS := base homematicbidcos homematicwired max insteon philipshue sonos miscellaneous user rpc cli events database scriptengine upnp paho.mqtt.c mqtt gd homegear

.PHONY: all clean help $(PROJECTS)

all: $(PROJECTS)

base: 
	@echo "==== Building base ($(config)) ===="
	@${MAKE} --no-print-directory -C . -f base.make

homematicbidcos: 
	@echo "==== Building homematicbidcos ($(config)) ===="
	@${MAKE} --no-print-directory -C . -f homematicbidcos.make

homematicwired: 
	@echo "==== Building homematicwired ($(config)) ===="
	@${MAKE} --no-print-directory -C . -f homematicwired.make

max: 
	@echo "==== Building max ($(config)) ===="
	@${MAKE} --no-print-directory -C . -f max.make

insteon: 
	@echo "==== Building insteon ($(config)) ===="
	@${MAKE} --no-print-directory -C . -f insteon.make

philipshue: 
	@echo "==== Building philipshue ($(config)) ===="
	@${MAKE} --no-print-directory -C . -f philipshue.make

sonos: 
	@echo "==== Building sonos ($(config)) ===="
	@${MAKE} --no-print-directory -C . -f sonos.make

miscellaneous: 
	@echo "==== Building miscellaneous ($(config)) ===="
	@${MAKE} --no-print-directory -C . -f miscellaneous.make

user: 
	@echo "==== Building user ($(config)) ===="
	@${MAKE} --no-print-directory -C . -f user.make

rpc: 
	@echo "==== Building rpc ($(config)) ===="
	@${MAKE} --no-print-directory -C . -f rpc.make

cli: 
	@echo "==== Building cli ($(config)) ===="
	@${MAKE} --no-print-directory -C . -f cli.make

events: 
	@echo "==== Building events ($(config)) ===="
	@${MAKE} --no-print-directory -C . -f events.make

database: 
	@echo "==== Building database ($(config)) ===="
	@${MAKE} --no-print-directory -C . -f database.make

scriptengine: 
	@echo "==== Building scriptengine ($(config)) ===="
	@${MAKE} --no-print-directory -C . -f scriptengine.make

upnp: 
	@echo "==== Building upnp ($(config)) ===="
	@${MAKE} --no-print-directory -C . -f upnp.make

paho.mqtt.c: 
	@echo "==== Building paho.mqtt.c ($(config)) ===="
	@${MAKE} --no-print-directory -C . -f paho.mqtt.c.make

mqtt: 
	@echo "==== Building mqtt ($(config)) ===="
	@${MAKE} --no-print-directory -C . -f mqtt.make

gd: 
	@echo "==== Building gd ($(config)) ===="
	@${MAKE} --no-print-directory -C . -f gd.make

homegear: rpc user cli events gd upnp mqtt database scriptengine base paho.mqtt.c
	@echo "==== Building homegear ($(config)) ===="
	@${MAKE} --no-print-directory -C . -f homegear.make

clean:
	@${MAKE} --no-print-directory -C . -f base.make clean
	@${MAKE} --no-print-directory -C . -f homematicbidcos.make clean
	@${MAKE} --no-print-directory -C . -f homematicwired.make clean
	@${MAKE} --no-print-directory -C . -f max.make clean
	@${MAKE} --no-print-directory -C . -f insteon.make clean
	@${MAKE} --no-print-directory -C . -f philipshue.make clean
	@${MAKE} --no-print-directory -C . -f sonos.make clean
	@${MAKE} --no-print-directory -C . -f miscellaneous.make clean
	@${MAKE} --no-print-directory -C . -f user.make clean
	@${MAKE} --no-print-directory -C . -f rpc.make clean
	@${MAKE} --no-print-directory -C . -f cli.make clean
	@${MAKE} --no-print-directory -C . -f events.make clean
	@${MAKE} --no-print-directory -C . -f database.make clean
	@${MAKE} --no-print-directory -C . -f scriptengine.make clean
	@${MAKE} --no-print-directory -C . -f upnp.make clean
	@${MAKE} --no-print-directory -C . -f paho.mqtt.c.make clean
	@${MAKE} --no-print-directory -C . -f mqtt.make clean
	@${MAKE} --no-print-directory -C . -f gd.make clean
	@${MAKE} --no-print-directory -C . -f homegear.make clean

help:
	@echo "Usage: make [config=name] [target]"
	@echo ""
	@echo "CONFIGURATIONS:"
	@echo "   release"
	@echo "   debug"
	@echo "   profiling"
	@echo ""
	@echo "TARGETS:"
	@echo "   all (default)"
	@echo "   clean"
	@echo "   base"
	@echo "   homematicbidcos"
	@echo "   homematicwired"
	@echo "   max"
	@echo "   insteon"
	@echo "   philipshue"
	@echo "   sonos"
	@echo "   miscellaneous"
	@echo "   user"
	@echo "   rpc"
	@echo "   cli"
	@echo "   events"
	@echo "   database"
	@echo "   scriptengine"
	@echo "   upnp"
	@echo "   paho.mqtt.c"
	@echo "   mqtt"
	@echo "   gd"
	@echo "   homegear"
	@echo ""
	@echo "For more information, see http://industriousone.com/premake/quick-start"
