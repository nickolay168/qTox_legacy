/*
    Copyright © 2015-2019 by The qTox Project Contributors

    This file is part of qTox, a Qt-based graphical interface for Tox.

    qTox is libre software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    qTox is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with qTox.  If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

#include "src/video/videomode.h"
#include "src/video/videosource.h"
#include <QFuture>
#include <QHash>
#include <QReadWriteLock>
#include <QString>
#include <QVector>
#include <atomic>

class CameraDevice;
struct AVCodecContext;
class Settings;

class CameraSource : public VideoSource
{
    Q_OBJECT

public:
    explicit CameraSource(Settings& settings);
    ~CameraSource();
    void setupDefault();
    bool isNone() const;

    // VideoSource interface
    void subscribe() override;
    void unsubscribe() override;

public slots:
    void setupDevice(const QString& deviceName_, const VideoMode& mode_);

signals:
    void deviceOpened();
    void openFailed();

private:
    void stream();

private slots:
    void openDevice();
    void closeDevice();

private:
    static const QString NONE;

    QFuture<void> streamFuture;
    QThread* deviceThread;

    QString deviceName;
    CameraDevice* device;
    VideoMode mode;
    AVCodecContext* cctx;
    // TODO: Remove when ffmpeg version will be bumped to the 3.1.0
    AVCodecContext* cctxOrig;
    int videoStreamIndex;

    QReadWriteLock deviceMutex;
    QReadWriteLock streamMutex;

    std::atomic_bool isNone_;
    std::atomic_int subscriptions;
    Settings& settings;
};

