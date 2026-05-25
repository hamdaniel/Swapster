# Makefile (MinGW / g++)
CXX := x86_64-w64-mingw32-g++
WINDRES_DEFAULT := C:/msys64/mingw64/bin/windres.exe
ifeq ($(wildcard $(WINDRES_DEFAULT)),)
WINDRES ?= windres
else
WINDRES ?= $(WINDRES_DEFAULT)
endif



ENC_CXXFLAGS   := -O2 -std=gnu++14
SERV_CXXFLAGS  := -O2 -std=gnu++14
CLNT_CXXFLAGS  := -O2 -std=gnu++17
SWAP_CXXFLAGS  := -O2 -std=c++11
INCLUDE_FLAGS  := -Iinclude -Isrc -Isrc/logic -Isrc/network

# Optional logging flag: make L=1
ifdef L
SERV_CXXFLAGS += -DLOG
endif

OBJ_DIR := obj
SRC_DIR := src
INC_DIR := include
SCRIPTS_DIR := scripts

ENC_O        := $(OBJ_DIR)/encryption.o
SWAP_O       := $(OBJ_DIR)/swapping.o
LAN_O        := $(OBJ_DIR)/lan.o

# Installer output
DIST_DIR := SwapsterDist
INSTALL_BAT := $(SCRIPTS_DIR)\installer.bat
INSTALL_PS1 := $(SCRIPTS_DIR)\install_swapster.ps1
WATCHDOG_PS1 := $(SCRIPTS_DIR)\swapster_watchdog.ps1
SWAPSTER := $(DIST_DIR)\swapster.exe
CLIENT := $(DIST_DIR)\controller.exe

# swapster version resource only
SWAPSTER_RC := swapster_version.rc
SWAPSTER_VER_O := $(OBJ_DIR)/swapster_version.o

# Link libs
SWAPSTER_LIBS := -lws2_32 -ladvapi32 -lgdi32
CLIENT_LIBS := -lws2_32 -ladvapi32 -liphlpapi

.PHONY: all clean rebuild dist

all: $(SWAPSTER) $(CLIENT) dist

$(OBJ_DIR):
	mkdir $(OBJ_DIR) 2>NUL || exit 0

$(DIST_DIR):
	mkdir $(DIST_DIR) 2>NUL || exit 0

# encryption.o
$(ENC_O): $(SRC_DIR)/logic/encryption.cpp $(INC_DIR)/encryption.h $(OBJ_DIR)
	$(CXX) $(ENC_CXXFLAGS) $(INCLUDE_FLAGS) -c $(SRC_DIR)/logic/encryption.cpp -o $(ENC_O)

# swapping.o
$(SWAP_O): $(SRC_DIR)/logic/swapping.cpp $(INC_DIR)/swapping.h $(INC_DIR)/encryption.h $(OBJ_DIR)
	$(CXX) $(SWAP_CXXFLAGS) $(INCLUDE_FLAGS) -c $(SRC_DIR)/logic/swapping.cpp -o $(SWAP_O)

# lan.o
$(LAN_O): $(SRC_DIR)/network/lan.cpp $(INC_DIR)/lan.h $(OBJ_DIR)
	$(CXX) $(CLNT_CXXFLAGS) $(INCLUDE_FLAGS) -c $(SRC_DIR)/network/lan.cpp -o $(LAN_O)

# server version resource -> .o
$(SWAPSTER_VER_O): $(SWAPSTER_RC) $(OBJ_DIR)
	$(WINDRES) --target=pe-x86-64 $(SWAPSTER_RC) -o $(SWAPSTER_VER_O)

# swapster.exe
$(SWAPSTER): $(SRC_DIR)/network/server.cpp $(ENC_O) $(SWAP_O) $(SWAPSTER_VER_O) $(DIST_DIR)
	$(CXX) $(SERV_CXXFLAGS) $(INCLUDE_FLAGS) -mwindows $(SRC_DIR)/network/server.cpp $(ENC_O) $(SWAP_O) $(SWAPSTER_VER_O) -o $(SWAPSTER) $(SWAPSTER_LIBS) -static-libgcc -static-libstdc++ -static

# controller.exe
$(CLIENT): $(SRC_DIR)/network/client.cpp $(ENC_O) $(DIST_DIR)
	$(CXX) $(CLNT_CXXFLAGS) $(INCLUDE_FLAGS) $(SRC_DIR)/network/client.cpp $(ENC_O) -o $(CLIENT) $(CLIENT_LIBS) -static-libgcc -static-libstdc++ -static

# ===== Distribution folder =====
dist:
	copy /Y "$(INSTALL_BAT)" "$(DIST_DIR)\" >NUL
	copy /Y "$(INSTALL_PS1)" "$(DIST_DIR)\" >NUL
	copy /Y "$(WATCHDOG_PS1)" "$(DIST_DIR)\" >NUL

clean:
	del /Q *.exe 2>NUL || exit 0
	del /Q *.o 2>NUL || exit 0
	rmdir /S /Q $(OBJ_DIR) 2>NUL || exit 0
	rmdir /S /Q $(DIST_DIR) 2>NUL || exit 0

rebuild: clean all