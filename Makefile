CXX = g++
CXXFLAGS = -Wall -Wextra -O2 -std=c++17 -Iinclude
LDFLAGS = 

TARGET_CLIENT = gitimg
TARGET_SERVER = gitimgd

SRC_DIR = src
OBJ_DIR = build
INC_DIR = include

SHARED_SRCS = $(SRC_DIR)/cdc_hash.cpp $(SRC_DIR)/packfile.cpp $(SRC_DIR)/manifest.cpp $(SRC_DIR)/protocol.cpp $(SRC_DIR)/repository.cpp $(SRC_DIR)/user.cpp
SHARED_OBJS = $(patsubst $(SRC_DIR)/%.cpp, $(OBJ_DIR)/%.o, $(SHARED_SRCS))

CLIENT_SRCS = $(SRC_DIR)/client_repo.cpp $(SRC_DIR)/main_client.cpp
CLIENT_OBJS = $(patsubst $(SRC_DIR)/%.cpp, $(OBJ_DIR)/%.o, $(CLIENT_SRCS))

SERVER_SRCS = $(SRC_DIR)/server_hub.cpp $(SRC_DIR)/main_server.cpp
SERVER_OBJS = $(patsubst $(SRC_DIR)/%.cpp, $(OBJ_DIR)/%.o, $(SERVER_SRCS))

all: directories $(TARGET_CLIENT) $(TARGET_SERVER)

directories:
	@mkdir -p $(OBJ_DIR)

$(TARGET_CLIENT): $(SHARED_OBJS) $(CLIENT_OBJS)
	$(CXX) $(CXXFLAGS) -o $(TARGET_CLIENT) $(SHARED_OBJS) $(CLIENT_OBJS) $(LDFLAGS)

$(TARGET_SERVER): $(SHARED_OBJS) $(SERVER_OBJS)
	$(CXX) $(CXXFLAGS) -o $(TARGET_SERVER) $(SHARED_OBJS) $(SERVER_OBJS) $(LDFLAGS)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -rf $(OBJ_DIR) $(TARGET_CLIENT) $(TARGET_SERVER)