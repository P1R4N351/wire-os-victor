.PHONY: vic-cloud vic-gateway

GO_EXE ?= /usr/local/go/bin/go
UPX_EXE ?= upx

all: vic-cloud 

go_deps:
	echo `$(GO_EXE) version` && cd $(PWD) && $(GO_EXE) mod download

vic-cloud: go_deps
	CGO_ENABLED=1 GOARM=7 GOARCH=arm CC=${HOME}/.anki/vicos-sdk/dist/5.3.0-r07/prebuilt/bin/arm-oe-linux-gnueabi-clang CXX=${HOME}/.anki/vicos-sdk/dist/5.3.0-r07/prebuilt/bin/arm-oe-linux-gnueabi-clang++ PKG_CONFIG_PATH="$(PWD)/armlibs/lib/pkgconfig" CGO_CFLAGS="-I$(PWD)/armlibs/include -I$(PWD)/armlibs/include/opus -I$(PWD)/armlibs/include/ogg" CGO_CXXFLAGS="-stdlib=libc++ -std=c++11" CGO_LDFLAGS="-L$(PWD)/armlibs/lib -L$(PWD)/armlibs/lib/arm-linux-gnueabi/android" $(GO_EXE) build -tags nolibopusfile,vicos -ldflags '-w -s -r /anki/lib' -o build/vic-cloud cloud/*

	$(UPX_EXE) build/vic-cloud


#vic-gateway: go_deps
#	CGO_ENABLED=1 GOARM=7 GOARCH=arm CC=${HOME}/.anki/vicos-sdk/dist/5.3.0-r07/prebuilt/bin/arm-oe-linux-gnueabi-clang CXX=${HOME}/.anki/vicos-sdk/dist/5.2.1-r06/prebuilt/bin/arm-oe-linux-gnueabi-clang++ PKG_CONFIG_PATH="$(PWD)/armlibs/lib/pkgconfig" CGO_CFLAGS="-I$(PWD)/armlibs/include -I$(PWD)/armlibs/include/opus -I$(PWD)/armlibs/include/ogg" CGO_CXXFLAGS="-stdlib=libc++ -std=c++11" CGO_LDFLAGS="-L$(PWD)/armlibs/lib -L$(PWD)/armlibs/lib/arm-linux-gnueabi/android" /usr/local/go/bin/go build -tags nolibopusfile,vicos -ldflags '-w -s -r /anki/lib' -o build/vic-gateway gateway/*.go
#
#	upx build/vic-gateway

