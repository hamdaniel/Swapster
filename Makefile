# Makefile (MinGW / g++)
CXX := g++
WINDRES := windres

ENC_CXXFLAGS   := -O2 -std=gnu++14
SERV_CXXFLAGS  := -O2 -std=gnu++14
CLNT_CXXFLAGS  := -O2 -std=gnu++14
SWAP_CXXFLAGS  := -O2 -std=c++11

ENC_O  := encryption.o
SWAP_O := swapster.o

SERVER := swapster.exe
CLIENT := client.exe

# Installer output
DIST_DIR := SwapsterInstallation
INSTALL_BAT := installation.bat

# Server version resource only
SERVER_RC := server_version.rc
SERVER_VER_O := server_version.o

# Link libs
SERVER_LIBS := -lws2_32 -ladvapi32 -lgdi32
CLIENT_LIBS := -lws2_32 -ladvapi32

.PHONY: all clean rebuild dist

all: $(SERVER) $(CLIENT) dist

# encryption.o
$(ENC_O): encryption.cpp encryption.h
	$(CXX) $(ENC_CXXFLAGS) -c encryption.cpp -o $(ENC_O)

# swapster.o
$(SWAP_O): swapster.cpp swapster.h encryption.h
	$(CXX) $(SWAP_CXXFLAGS) -c swapster.cpp -o $(SWAP_O)

# server version resource -> .o
$(SERVER_VER_O): $(SERVER_RC)
	$(WINDRES) $(SERVER_RC) -o $(SERVER_VER_O)

# server.exe (no console)
$(SERVER): server.cpp $(ENC_O) $(SWAP_O) $(SERVER_VER_O)
	$(CXX) $(SERV_CXXFLAGS) -mwindows server.cpp $(ENC_O) $(SWAP_O) $(SERVER_VER_O) -o $(SERVER) $(SERVER_LIBS)

# client.exe
$(CLIENT): client.cpp $(ENC_O)
	$(CXX) $(CLNT_CXXFLAGS) client.cpp $(ENC_O) -o $(CLIENT) $(CLIENT_LIBS)

# ===== Distribution folder =====
dist:
	mkdir $(DIST_DIR) 2>NUL || exit 0
	copy /Y $(SERVER) $(DIST_DIR)\ >NUL
	copy /Y $(INSTALL_BAT) $(DIST_DIR)\ >NUL

clean:
	del /Q *.exe *.o 2>NUL || exit 0
	rmdir /S /Q $(DIST_DIR) 2>NUL || exit 0

rebuild: clean all