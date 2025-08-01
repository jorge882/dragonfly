# Build DragonflyDB From Source

## Running the server

Dragonfly runs on linux. We advise running it on linux version 5.11 or later
but you can also run Dragonfly on older kernels as well.

> :warning: **Dragonfly releases are compiled with LTO (link time optimization)**:
  Depending on the workload this can notably improve performance. If you want to
  benchmark Dragonfly or use it in production, you should enable LTO by giving
  `blaze.sh` the `-DCMAKE_CXX_FLAGS="-flto"` argument.

## Step 1 - install dependencies

On Debian/Ubuntu:

```bash
sudo apt install ninja-build libunwind-dev libboost-context-dev libssl-dev \
     autoconf-archive libtool cmake g++ bison  zlib1g-dev
```

On Fedora:

```bash
sudo dnf install -y automake boost-devel g++ git cmake libtool ninja-build \
     openssl-devel libunwind-devel autoconf-archive patch bison libstdc++-static
```

On openSUSE:

```bash
sudo zypper install automake boost-devel gcc-c++ git cmake libtool ninja \
     openssl-devel libunwind-devel autoconf-archive patch bison \
     libboost_context-devel libboost_system-devel
```

On FreeBSD:

```bash
pkg install -y git bash cmake ninja libunwind boost-libs autoconf automake libtool gmake bison
```

## Step 2 - clone the project

```bash
git clone --recursive https://github.com/dragonflydb/dragonfly && cd dragonfly
```

## Step 3 - configure & build it

```bash
# Configure the build
./helio/blaze.sh -release

# Build
cd build-opt && ninja dragonfly

```

### Build options

| Option | Description |
|--|--|
| WITH_AWS | Include AWS client. Required for cloud snapshots |
| WITH_SEARCH | Include Search module |
| WITH_COLLECTION_CMDS | Include commands for collections (SET, HSET, ZSET) |
| WITH_EXTENSION_CMDS | Include extension commands (Bloom, HLL, JSON, ...) |

Minimal debug build:

```bash
./helio/blaze.sh -DWITH_AWS=OFF -DWITH_SEARCH=OFF -DWITH_COLLECTION_CMDS=OFF -DWITH_EXTENSION_CMDS=OFF
```

## Step 4 - voilà

```bash
# Run
./dragonfly --alsologtostderr

```

Dragonfly DB will answer to both `http` and `redis` requests out of the box!

You can use `redis-cli` to connect to `localhost:6379` or open a browser and visit `http://localhost:6379`

## Step 5

Connect with a redis client

```bash
redis-cli
127.0.0.1:6379> set hello world
OK
127.0.0.1:6379> keys *
1) "hello"
127.0.0.1:6379> get hello
"world"
127.0.0.1:6379>
```

## Step 6

Continue being great and build your app with the power of DragonflyDB!
