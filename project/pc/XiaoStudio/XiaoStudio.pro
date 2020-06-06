QT       += core gui widgets

greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

CONFIG += c++14

# The following define makes your compiler emit warnings if you use
# any Qt feature that has been marked deprecated (the exact warnings
# depend on your compiler). Please consult the documentation of the
# deprecated API in order to know how to port your code away from it.
DEFINES += QT_DEPRECATED_WARNINGS

# You can also make your code fail to compile if it uses deprecated APIs.
# In order to do so, uncomment the following line.
# You can also select to disable deprecated APIs only up to a certain version of Qt.
#DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000    # disables all the APIs deprecated before Qt 6.0.0

# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target

# SDL库所需的宏
#DEFINES += SDL_MAIN_HANDLED
DEFINES += _CRT_SECURE_NO_WARNINGS

INCLUDEPATH += $$PWD/../../../thirdparty/ffmpeg/include
INCLUDEPATH += $$PWD/../../../thirdparty/SDL2-2.0.12/include
INCLUDEPATH += $$PWD/../../../source/MediaPlayer/include
INCLUDEPATH += $$PWD/source/

win32{
    LIBS += -L$$PWD/../../../thirdparty/ffmpeg/lib/win32 -lavcodec -lavdevice -lavfilter -lavformat -lavutil -lpostproc -lswresample -lswscale
    LIBS += -L$$PWD/../../../thirdparty/SDL2-2.0.12/lib/win32/x64 -lSDL2 -lSDL2main
}
unix{

}
mac{

}

SOURCES += \
    ../../../source/MediaPlayer/src/FDecoder.cpp \
    ../../../source/MediaPlayer/src/FMediaPlayer.cpp \
    main.cpp \
    mainwindow.cpp \
    source/QMediaPlayer.cpp \
    ui/QSettings.cpp

HEADERS += \
    ../../../source/MediaPlayer/include/FDecoder.h \
    ../../../source/MediaPlayer/include/FMediaPlayer.h \
    mainwindow.h \
    source/QMediaPlayer.h \
    ui/QSettings.h

FORMS += \
    mainwindow.ui \
    ui/QSettings.ui

DISTFILES += \
    ui/res/icon/ic_lock_ringer_off_alpha.png \
    ui/res/icon/ic_lock_ringer_on_alpha.png \
    ui/res/icon/ic_menu_blocked_user.png \
    ui/res/icon/ic_menu_cc_am.png \
    ui/res/icon/ic_menu_delete.png \
    ui/res/icon/ic_settings.png


