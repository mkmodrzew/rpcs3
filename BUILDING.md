# Building

Only Windows and Linux are officially supported for building. However, various other platforms are capable of building RPCS3.
Other instructions may be found [here](https://wiki.rpcs3.net/index.php?title=Building).

## Setup your environment

### Windows 7 or later

* [CMake 3.14.1+](https://www.cmake.org/download/) (add to PATH)
* [Python 3.3+](https://www.python.org/downloads/) (add to PATH)
* [Qt 5.15.2](https://www.qt.io/download-qt-installer)
* [Visual Studio 2019](https://visualstudio.microsoft.com/thank-you-downloading-visual-studio/?sku=Community)
* [Vulkan SDK 1.1.126+](https://vulkan.lunarg.com/sdk/home) (See "Install the SDK" [here](https://vulkan.lunarg.com/doc/sdk/latest/windows/getting_started.html))

**Either add the** `QTDIR` **environment variable, e.g.** `<QtInstallFolder>\5.15.2\msvc2019_64\` **, or use the [Visual Studio Qt Plugin](https://marketplace.visualstudio.com/items?itemName=TheQtCompany.QtVisualStudioTools2019)**

### Linux

These are the essentials tools to build RPCS3 on Linux. Some of them can be installed through your favorite package manager.

* Clang 11+ or GCC 9+
* [CMake 3.14.1+](https://www.cmake.org/download/)
* [Qt 5.15.2](https://www.qt.io/download-qt-installer)
* [Vulkan SDK 1.1.126+](https://vulkan.lunarg.com/sdk/home) (See "Install the SDK" [here](https://vulkan.lunarg.com/doc/sdk/latest/linux/getting_started.html))
* [SDL2](https://www.libsdl.org/download-2.0.php) (for the FAudio backend)

**If you have an NVIDIA GPU, you may need to install the libglvnd package.**

#### Arch Linux

    sudo pacman -S glew openal cmake vulkan-validation-layers qt5-base qt5-declarative sdl2

#### Debian & Ubuntu

    sudo apt-get install build-essential libasound2-dev libpulse-dev libopenal-dev libglew-dev zlib1g-dev libedit-dev libvulkan-dev libudev-dev git libevdev-dev libsdl2-2.0 libsdl2-dev

Ubuntu is usually horrendously out of date, and some packages need to be downloaded by hand. This part is for Qt, GCC, Vulkan, and CMake
##### Qt PPA

Ubuntu usually does not have a new enough Qt package to suit rpcs3's needs. There is a PPA available to work around this. Run the following:
```
. /etc/os-release
sudo add-apt-repository ppa:beineri/opt-qt-5.15.2-$UBUNTU_CODENAME
sudo apt-get update
sudo apt-get install qt515-meta-minimal qt515svg
. /opt/qt515/bin/qt515-env.sh >/dev/null 2>&1
```

##### GCC 9.x installation

If the `gcc-9` package is not available on your system, use the following commands
```
sudo add-apt-repository ppa:ubuntu-toolchain-r/test
sudo apt-get update
sudo apt-get install gcc-9 g++-9
```

You can either use `update-alternatives` to setup `gcc-9`/`g++-9` as your default compilers or prefix any `cmake` command by `CXX=g++-9 CC=gcc-9 ` to use it.

##### Vulkan SDK

For Ubuntu systems, it is strongly recommended to use the PPA from [LunarG](https://packages.lunarg.com/) which will provide a compatible Vulkan SDK to compile RPCS3. If your Vulkan SDK is older, it can lead to compilation errors.
```
. /etc/os-release
wget -qO - https://packages.lunarg.com/lunarg-signing-key-pub.asc | sudo apt-key add -
sudo wget -qO /etc/apt/sources.list.d/lunarg-vulkan-1.2.148-$UBUNTU_CODENAME.list https://packages.lunarg.com/vulkan/1.2.148/lunarg-vulkan-1.2.148-$UBUNTU_CODENAME.list
sudo apt update
sudo apt install vulkan-sdk
```

##### CMake
```
. /etc/os-release
wget -O - https://apt.kitware.com/keys/kitware-archive-latest.asc 2>/dev/null | sudo apt-key add -
sudo apt-add-repository "deb https://apt.kitware.com/ubuntu/ $UBUNTU_CODENAME main"
sudo apt-get update
sudo apt-get install kitware-archive-keyring
sudo apt-key --keyring /etc/apt/trusted.gpg del C1F34CDD40CD72DA
sudo apt-get install cmake
```

#### Fedora

    sudo dnf install alsa-lib-devel cmake glew glew-devel libatomic libevdev-devel libudev-devel openal-devel qt5-qtbase-devel qt5-qtbase-private-devel vulkan-devel

#### OpenSUSE

    sudo zypper install git cmake libasound2 libpulse-devel openal-soft-devel glew-devel zlib-devel libedit-devel vulkan-devel libudev-devel libqt5-qtbase-devel libqt5gui-private-headers-devel libevdev-devel

## Setup the project

Clone and initialize the repository

```
git clone https://github.com/RPCS3/rpcs3.git
cd rpcs3
git submodule update --init
```

### Windows

#### Configuring the Qt plugin (if used)

1) Go to the Qt5 menu and edit Qt5 options.
2) Add the path to your Qt installation with compiler e.g. `<QtInstallFolder>\5.15.2\msvc2019_64`.
3) While selecting the rpcs3qt project, go to Qt5->Project Setting and select the version you added.

#### Building the projects

Open `rpcs3.sln`. The recommended build configuration is `Release`. (On older revisions: `Release - LLVM`)

You may want to download the precompiled [LLVM libs](https://github.com/RPCS3/llvm-mirror/releases/download/custom-build-win/llvmlibs_mt.7z) and extract them to the root rpcs3 folder (which contains `rpcs3.sln`), as well as download and extract the [additional libs](https://github.com/RPCS3/glslang/releases/download/custom-build-win/glslanglibs_mt.7z) to `lib\%CONFIGURATION%-x64\` to speed up compilation time (unoptimised/debug libs are currently not available precompiled).

If you're not using the precompiled libs, build the following projects in *__BUILD_BEFORE* folder by right-clicking on a project > *Build*.:
* glslang-build
* **Either** llvm_build **or** llvm_build_clang_cl
* spirv-tools-build

Afterwards:

`Build > Build Solution`

### Linux

While still in the project root:

1) `cd .. && mkdir rpcs3_build && cd rpcs3_build`
2) `cmake ../rpcs3/ && make` or `CXX=g++-9 CC=gcc-9 cmake ../rpcs3/ && make` to force these compilers
3) Run RPCS3 with `./bin/rpcs3`

When using GDB, configure it to ignore SIGSEGV signal (`handle SIGSEGV nostop noprint`).
If desired, use the various build options in [CMakeLists](https://github.com/RPCS3/rpcs3/blob/master/CMakeLists.txt).
