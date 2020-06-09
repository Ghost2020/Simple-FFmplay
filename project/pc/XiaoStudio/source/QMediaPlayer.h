﻿/**
* \@brief Author			Ghost Chen
* \@brief Email				cxx2020@outlook.com
* \@brief Date				2020/05/20
* \@brief File				QMediaPlayer.h
* \@brief Desc:				MediaPlayer
*/
#ifndef QMEDIAPLAYER_H
#define QMEDIAPLAYER_H

#include <QKeyEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QTimer>
#include <QMenu>

//#include <QWindow>
//#include <QScreen>

#include "FMediaPlayer.h"

//#if defined(Q_OS_WIN32)
#include <QtWidgets/QOpenGLWidget>
class QMediaPlayer : public  QOpenGLWidget
//#elif defined (Q_OS_MACOS)
//#include <QMacNativeWidget>
//class QMediaPlayer : public QMacNativeWidget
//#endif
{
Q_OBJECT
public :
    QMediaPlayer(QWidget* parent = nullptr);
    ~QMediaPlayer() override;

public:
    void openStream();

public slots:
    void onShowMenu();
    void onBeginPlay();
    void OnPausePlay();
    void OnToggleMute();
    void onStopPlay();
    void onRecord();
    void onQuit();

    void ON_TEST();

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void resizeEvent(QResizeEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    int64_t m_nLast_mouse_left_click = 0;
    QString m_sURL = "";
    QMenu* m_pMenu = nullptr;
    QTimer* m_pTimer = nullptr;
    std::unique_ptr<FMediaPlayer> m_pCorePlayer = nullptr;
};

#endif // QMEDIAPLAYER_H
