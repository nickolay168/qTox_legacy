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


#include "corefile.h"
#include "core.h"
#include "toxfile.h"
#include "toxstring.h"
#include "src/persistence/settings.h"
#include "src/model/status.h"
#include "src/model/toxclientstandards.h"
#include "util/compatiblerecursivemutex.h"
#include "util/toxcoreerrorparser.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QThread>

#include <tox/tox.h>

#include <cassert>
#include <memory>

/**
 * @class CoreFile
 * @brief Manages the file transfer service of toxcore
 */

CoreFilePtr CoreFile::makeCoreFile(Core *core, Tox *tox, CompatibleRecursiveMutex &coreLoopLock)
{
    assert(core != nullptr);
    assert(tox != nullptr);
    connectCallbacks(*tox);
    CoreFilePtr result = CoreFilePtr{new CoreFile{tox, coreLoopLock}};
    connect(core, &Core::friendStatusChanged, result.get(), &CoreFile::onConnectionStatusChanged);

    return result;
}

CoreFile::CoreFile(Tox *core_, CompatibleRecursiveMutex &coreLoopLock_)
    : tox{core_}
    , coreLoopLock{&coreLoopLock_}
{
}

/**
 * @brief Get corefile iteration interval.
 *
 * tox_iterate calls to get good file transfer performances
 * @return The maximum amount of time in ms that Core should wait between two tox_iterate() calls.
 */
unsigned CoreFile::corefileIterationInterval()
{
    /*
       Sleep at most 1000ms if we have no FT, 10 for user FTs
       There is no real difference between 10ms sleep and 50ms sleep when it
       comes to CPU usage – just keep the CPU usage low when there are no file
       transfers, and speed things up when there is an ongoing file transfer.
    */
    constexpr unsigned fileInterval = 10, idleInterval = 1000;

    for (ToxFile& file : fileMap) {
        if (file.status == ToxFile::TRANSMITTING) {
            return fileInterval;
        }
    }
    return idleInterval;
}

void CoreFile::connectCallbacks(Tox &tox)
{
    // be careful not to to reconnect already used callbacks here
    tox_callback_file_chunk_request(&tox, CoreFile::onFileDataCallback);
    tox_callback_file_recv(&tox, CoreFile::onFileReceiveCallback);
    tox_callback_file_recv_chunk(&tox, CoreFile::onFileRecvChunkCallback);
    tox_callback_file_recv_control(&tox, CoreFile::onFileControlCallback);
}

void CoreFile::sendAvatarFile(uint32_t friendId, const QByteArray& data)
{
    QMutexLocker locker{coreLoopLock};

    uint64_t filesize = 0;
    uint8_t *file_id = nullptr;
    uint8_t *file_name = nullptr;
    size_t nameLength = 0;
    uint8_t avatarHash[TOX_HASH_LENGTH];
    if (!data.isEmpty()) {
        static_assert(TOX_HASH_LENGTH <= TOX_FILE_ID_LENGTH, "TOX_HASH_LENGTH > TOX_FILE_ID_LENGTH!");
        tox_hash(avatarHash, reinterpret_cast<const uint8_t*>(data.data()), data.size());
        filesize = data.size();
        file_id = avatarHash;
        file_name = avatarHash;
        nameLength = TOX_HASH_LENGTH;
    }
    Tox_Err_File_Send error;
    const uint32_t fileNum = tox_file_send(tox, friendId, TOX_FILE_KIND_AVATAR, filesize,
                                    file_id, file_name, nameLength, &error);
    if (!PARSE_ERR(error)) {
        return;
    }

    ToxFile file{fileNum, friendId, "", "", filesize, ToxFile::SENDING};
    file.fileKind = TOX_FILE_KIND_AVATAR;
    file.avatarData = data;
    file.resumeFileId.resize(TOX_FILE_ID_LENGTH);
    Tox_Err_File_Get fileGetErr;
    tox_file_get_file_id(tox, friendId, fileNum, reinterpret_cast<uint8_t*>(file.resumeFileId.data()),
                         &fileGetErr);
    if (!PARSE_ERR(fileGetErr)) {
        return;
    }
    addFile(friendId, fileNum, file);
}

void CoreFile::sendFile(uint32_t friendId, QString filename, QString filePath,
                        long long filesize)
{
    QMutexLocker locker{coreLoopLock};

    ToxString fileName(filename);
    Tox_Err_File_Send sendErr;
    uint32_t fileNum = tox_file_send(tox, friendId, TOX_FILE_KIND_DATA, filesize,
                                     nullptr, fileName.data(), fileName.size(), &sendErr);

    if (!PARSE_ERR(sendErr)) {
        emit fileSendFailed(friendId, fileName.getQString());
        return;
    }
    qDebug() << QString("sendFile: Created file sender %1 with friend %2").arg(fileNum).arg(friendId);

    ToxFile file{fileNum, friendId, fileName.getQString(), filePath, static_cast<uint64_t>(filesize), ToxFile::SENDING};
    file.resumeFileId.resize(TOX_FILE_ID_LENGTH);
    Tox_Err_File_Get fileGetErr;
    tox_file_get_file_id(tox, friendId, fileNum, reinterpret_cast<uint8_t*>(file.resumeFileId.data()),
                         &fileGetErr);
    if (!PARSE_ERR(fileGetErr)) {
        return;
    }
    if (!file.open(false)) {
        qWarning() << QString("sendFile: Can't open file, error: %1").arg(file.file->errorString());
    }

    addFile(friendId, fileNum, file);

    emit fileSendStarted(file);
}

void CoreFile::pauseResumeFile(uint32_t friendId, uint32_t fileId)
{
    QMutexLocker locker{coreLoopLock};

    ToxFile* file = findFile(friendId, fileId);
    if (!file) {
        qWarning("pauseResumeFileSend: No such file in queue");
        return;
    }

    if (file->status != ToxFile::TRANSMITTING && file->status != ToxFile::PAUSED) {
        qWarning() << "pauseResumeFileSend: File is stopped";
        return;
    }

    file->pauseStatus.localPauseToggle();

    if (file->pauseStatus.paused()) {
        file->status = ToxFile::PAUSED;
        file->progress.resetSpeed();
        emit fileTransferPaused(*file);
    } else {
        file->status = ToxFile::TRANSMITTING;
        emit fileTransferAccepted(*file);
    }

    Tox_Err_File_Control err;
    if (file->pauseStatus.localPaused()) {
        tox_file_control(tox, file->friendId, file->fileNum, TOX_FILE_CONTROL_PAUSE,
                         &err);
    } else {
        tox_file_control(tox, file->friendId, file->fileNum, TOX_FILE_CONTROL_RESUME,
                         &err);
    }
    PARSE_ERR(err);
}

void CoreFile::cancelFileSend(uint32_t friendId, uint32_t fileId)
{
    QMutexLocker locker{coreLoopLock};

    ToxFile* file = findFile(friendId, fileId);
    if (!file) {
        qWarning("cancelFileSend: No such file in queue");
        return;
    }

    Tox_Err_File_Control err;
    tox_file_control(tox, file->friendId, file->fileNum, TOX_FILE_CONTROL_CANCEL, &err);
    if (!PARSE_ERR(err)) {
        return;
    }
    file->status = ToxFile::CANCELED;
    emit fileTransferCancelled(*file);
    removeFile(friendId, fileId);
}

void CoreFile::cancelFileRecv(uint32_t friendId, uint32_t fileId)
{
    QMutexLocker locker{coreLoopLock};

    ToxFile* file = findFile(friendId, fileId);
    if (!file) {
        qWarning("cancelFileRecv: No such file in queue");
        return;
    }
    Tox_Err_File_Control err;
    tox_file_control(tox, file->friendId, file->fileNum, TOX_FILE_CONTROL_CANCEL, &err);
    if (!PARSE_ERR(err)) {
        return;
    }
    file->status = ToxFile::CANCELED;
    emit fileTransferCancelled(*file);
    removeFile(friendId, fileId);
}

void CoreFile::rejectFileRecvRequest(uint32_t friendId, uint32_t fileId)
{
    QMutexLocker locker{coreLoopLock};

    ToxFile* file = findFile(friendId, fileId);
    if (!file) {
        qWarning("rejectFileRecvRequest: No such file in queue");
        return;
    }
    Tox_Err_File_Control err;
    tox_file_control(tox, file->friendId, file->fileNum, TOX_FILE_CONTROL_CANCEL, &err);
    if (!PARSE_ERR(err)) {
        return;
    }
    file->status = ToxFile::CANCELED;
    emit fileTransferCancelled(*file);
    removeFile(friendId, fileId);
}

void CoreFile::acceptFileRecvRequest(uint32_t friendId, uint32_t fileId, QString path)
{
    QMutexLocker locker{coreLoopLock};

    ToxFile* file = findFile(friendId, fileId);
    if (!file) {
        qWarning("acceptFileRecvRequest: No such file in queue");
        return;
    }
    file->setFilePath(path);
    if (!file->open(true)) {
        qWarning() << "acceptFileRecvRequest: Unable to open file";
        return;
    }
    Tox_Err_File_Control err;
    tox_file_control(tox, file->friendId, file->fileNum, TOX_FILE_CONTROL_RESUME, &err);
    if (!PARSE_ERR(err)) {
        return;
    }
    file->status = ToxFile::TRANSMITTING;
    emit fileTransferAccepted(*file);
}

ToxFile* CoreFile::findFile(uint32_t friendId, uint32_t fileId)
{
    QMutexLocker locker{coreLoopLock};

    uint64_t key = getFriendKey(friendId, fileId);
    if (fileMap.contains(key)) {
        return &fileMap[key];
    }

    qWarning() << "findFile: File transfer with ID" << friendId << ':' << fileId << "doesn't exist";
    return nullptr;
}

void CoreFile::addFile(uint32_t friendId, uint32_t fileId, const ToxFile& file)
{
    uint64_t key = getFriendKey(friendId, fileId);

    if (fileMap.contains(key)) {
        qWarning() << "addFile: Overwriting existing file transfer with same ID" << friendId << ':'
                   << fileId;
    }

    fileMap.insert(key, file);
}

void CoreFile::removeFile(uint32_t friendId, uint32_t fileId)
{
    uint64_t key = getFriendKey(friendId, fileId);
    if (!fileMap.contains(key)) {
        qWarning() << "removeFile: No such file in queue";
        return;
    }
    fileMap[key].file->close();
    fileMap.remove(key);
}

QString CoreFile::getCleanFileName(QString filename)
{
    QRegularExpression regex{QStringLiteral(R"([<>:"/\\|?])")};
    filename.replace(regex, "_");

    return filename;
}

void CoreFile::onFileReceiveCallback(Tox* tox, uint32_t friendId, uint32_t fileId, uint32_t kind,
                                     uint64_t filesize, const uint8_t* fname, size_t fnameLen,
                                     void* vCore)
{
    Core* core = static_cast<Core*>(vCore);
    CoreFile* coreFile = core->getCoreFile();
    auto filename = ToxString(fname, fnameLen);
    const ToxPk friendPk = core->getFriendPublicKey(friendId);

    if (kind == TOX_FILE_KIND_AVATAR) {
        if (!filesize) {
            qDebug() << QString("Received empty avatar request %1:%2").arg(friendId).arg(fileId);
            // Avatars of size 0 means explicitely no avatar
            Tox_Err_File_Control err;
            tox_file_control(tox, friendId, fileId, TOX_FILE_CONTROL_CANCEL, &err);
            PARSE_ERR(err);
            emit core->friendAvatarRemoved(core->getFriendPublicKey(friendId));
            return;
        }
        if (!ToxClientStandards::IsValidAvatarSize(filesize)) {
            qWarning() <<
                QString("Received avatar request from %1 with size %2.").arg(friendId).arg(filesize) +
                QString(" The max size allowed for avatars is %3. Cancelling transfer.").arg(ToxClientStandards::MaxAvatarSize);
            Tox_Err_File_Control err;
            tox_file_control(tox, friendId, fileId, TOX_FILE_CONTROL_CANCEL, &err);
            PARSE_ERR(err);
            return;
        }
        static_assert(TOX_HASH_LENGTH <= TOX_FILE_ID_LENGTH,
                        "TOX_HASH_LENGTH > TOX_FILE_ID_LENGTH!");
        uint8_t avatarHash[TOX_FILE_ID_LENGTH];
        Tox_Err_File_Get fileGetErr;
        tox_file_get_file_id(tox, friendId, fileId, avatarHash, &fileGetErr);
        if (!PARSE_ERR(fileGetErr)) {
            return;
        }
        QByteArray avatarBytes{static_cast<const char*>(static_cast<const void*>(avatarHash)),
                                TOX_HASH_LENGTH};
        emit core->fileAvatarOfferReceived(friendId, fileId, avatarBytes, filesize);
        return;
    }
    #ifdef Q_OS_WIN
    const auto cleanFileName = CoreFile::getCleanFileName(filename.getQString());
    if (cleanFileName != filename.getQString()) {
        qDebug() << QStringLiteral("Cleaned filename");
        filename = ToxString(cleanFileName);
        emit coreFile->fileNameChanged(friendPk);
    } else {
        qDebug() << QStringLiteral("filename already clean");
    }
    #endif
    qDebug() << QString("Received file request %1:%2 kind %3").arg(friendId).arg(fileId).arg(kind);

    ToxFile file{fileId, friendId, filename.getQString(), "", filesize, ToxFile::RECEIVING};
    file.fileKind = kind;
    file.resumeFileId.resize(TOX_FILE_ID_LENGTH);
    Tox_Err_File_Get fileGetErr;
    tox_file_get_file_id(tox, friendId, fileId, reinterpret_cast<uint8_t*>(file.resumeFileId.data()),
                         &fileGetErr);
    if (!PARSE_ERR(fileGetErr)) {
        return;
    }
    coreFile->addFile(friendId, fileId, file);
    if (kind != TOX_FILE_KIND_AVATAR) {
        emit coreFile->fileReceiveRequested(file);
    }
}

// TODO(sudden6): This whole method is a mess but needed to get stuff working for now
void CoreFile::handleAvatarOffer(uint32_t friendId, uint32_t fileId, bool accept, uint64_t filesize)
{
    if (!accept) {
        // If it's an avatar but we already have it cached, cancel
        qDebug() << QString("Received avatar request %1:%2. Rejected since it is in cache.")
                        .arg(friendId)
                        .arg(fileId);
        Tox_Err_File_Control err;
        tox_file_control(tox, friendId, fileId, TOX_FILE_CONTROL_CANCEL, &err);
        PARSE_ERR(err);
        return;
    }

    Tox_Err_File_Control err;
    tox_file_control(tox, friendId, fileId, TOX_FILE_CONTROL_RESUME, &err);
    if (!PARSE_ERR(err)) {
        return;
    }
    // It's an avatar and we don't have it, autoaccept the transfer
    qDebug() << QString("Received avatar request %1:%2. Accepted.")
                    .arg(friendId)
                    .arg(fileId);

    ToxFile file{fileId, friendId, "<avatar>", "", filesize, ToxFile::RECEIVING};
    file.fileKind = TOX_FILE_KIND_AVATAR;
    file.resumeFileId.resize(TOX_FILE_ID_LENGTH);
    Tox_Err_File_Get getErr;
    tox_file_get_file_id(tox, friendId, fileId, reinterpret_cast<uint8_t*>(file.resumeFileId.data()),
                         &getErr);
    if (!PARSE_ERR(getErr)) {
        return;
    }
    addFile(friendId, fileId, file);
}

void CoreFile::onFileControlCallback(Tox* tox, uint32_t friendId, uint32_t fileId,
                                     Tox_File_Control control, void* vCore)
{
    std::ignore = tox;
    Core* core = static_cast<Core*>(vCore);
    CoreFile* coreFile = core->getCoreFile();
    ToxFile* file = coreFile->findFile(friendId, fileId);
    if (!file) {
        qWarning("onFileControlCallback: No such file in queue");
        return;
    }

    if (control == TOX_FILE_CONTROL_CANCEL) {
        if (file->fileKind != TOX_FILE_KIND_AVATAR)
            qDebug() << "File transfer" << friendId << ":" << fileId << "cancelled by friend";
        file->status = ToxFile::CANCELED;
        emit coreFile->fileTransferCancelled(*file);
        coreFile->removeFile(friendId, fileId);
    } else if (control == TOX_FILE_CONTROL_PAUSE) {
        qDebug() << "onFileControlCallback: Received pause for file " << friendId << ":" << fileId;
        file->pauseStatus.remotePause();
        file->status = ToxFile::PAUSED;
        emit coreFile->fileTransferRemotePausedUnpaused(*file, true);
    } else if (control == TOX_FILE_CONTROL_RESUME) {
        if (file->direction == ToxFile::SENDING && file->fileKind == TOX_FILE_KIND_AVATAR)
            qDebug() << "Avatar transfer" << fileId << "to friend" << friendId << "accepted";
        else
            qDebug() << "onFileControlCallback: Received resume for file " << friendId << ":" << fileId;
        file->pauseStatus.remoteResume();
        file->status = file->pauseStatus.paused() ? ToxFile::PAUSED : ToxFile::TRANSMITTING;
        emit coreFile->fileTransferRemotePausedUnpaused(*file, false);
    } else {
        qWarning() << "Unhandled file control " << control << " for file " << friendId << ':' << fileId;
    }
}

void CoreFile::onFileDataCallback(Tox* tox, uint32_t friendId, uint32_t fileId, uint64_t pos,
                                  size_t length, void* vCore)
{

    Core* core = static_cast<Core*>(vCore);
    CoreFile* coreFile = core->getCoreFile();
    ToxFile* file = coreFile->findFile(friendId, fileId);
    if (!file) {
        qWarning("onFileDataCallback: No such file in queue");
        return;
    }

    // If we reached EOF, ack and cleanup the transfer
    if (!length) {
        file->status = ToxFile::FINISHED;
        if (file->fileKind != TOX_FILE_KIND_AVATAR) {
            emit coreFile->fileTransferFinished(*file);
        }
        coreFile->removeFile(friendId, fileId);
        return;
    }

    std::unique_ptr<uint8_t[]> data(new uint8_t[length]);
    int64_t nread;

    if (file->fileKind == TOX_FILE_KIND_AVATAR) {
        QByteArray chunk = file->avatarData.mid(pos, length);
        nread = chunk.size();
        memcpy(data.get(), chunk.data(), nread);
    } else {
        file->file->seek(pos);
        nread = file->file->read(reinterpret_cast<char*>(data.get()), length);
        if (nread <= 0) {
            qWarning("onFileDataCallback: Failed to read from file");
            file->status = ToxFile::CANCELED;
            emit coreFile->fileTransferCancelled(*file);
            Tox_Err_File_Send_Chunk err;
            tox_file_send_chunk(tox, friendId, fileId, pos, nullptr, 0, &err);
            PARSE_ERR(err);
            coreFile->removeFile(friendId, fileId);
            return;
        }
        file->progress.addSample(file->progress.getBytesSent() + length);
        file->hashGenerator->addData(QByteArrayView(data.get()));
    }

    Tox_Err_File_Send_Chunk err;
    tox_file_send_chunk(tox, friendId, fileId, pos, data.get(), nread, &err);
    if (!PARSE_ERR(err)) {
        return;
    }
    if (file->fileKind != TOX_FILE_KIND_AVATAR) {
        emit coreFile->fileTransferInfo(*file);
    }
}

void CoreFile::onFileRecvChunkCallback(Tox* tox, uint32_t friendId, uint32_t fileId, uint64_t position,
                                       const uint8_t* data, size_t length, void* vCore)
{
    Core* core = static_cast<Core*>(vCore);
    CoreFile* coreFile = core->getCoreFile();
    ToxFile* file = coreFile->findFile(friendId, fileId);
    if (!file) {
        qWarning("onFileRecvChunkCallback: No such file in queue");
        Tox_Err_File_Control err;
        tox_file_control(tox, friendId, fileId, TOX_FILE_CONTROL_CANCEL, &err);
        PARSE_ERR(err);
        return;
    }

    if (file->progress.getBytesSent() != position) {
        qWarning("onFileRecvChunkCallback: Received a chunk out-of-order, aborting transfer");
        if (file->fileKind != TOX_FILE_KIND_AVATAR) {
            file->status = ToxFile::CANCELED;
            emit coreFile->fileTransferCancelled(*file);
        }
        Tox_Err_File_Control err;
        tox_file_control(tox, friendId, fileId, TOX_FILE_CONTROL_CANCEL, &err);
        PARSE_ERR(err);
        coreFile->removeFile(friendId, fileId);
        return;
    }

    if (!length) {
        file->status = ToxFile::FINISHED;
        if (file->fileKind == TOX_FILE_KIND_AVATAR) {
            QPixmap pic;
            if (pic.loadFromData(file->avatarData)) {
                qDebug() << "Got" << file->avatarData.size() << "bytes of avatar data from" << friendId;
                emit core->friendAvatarChanged(core->getFriendPublicKey(friendId), file->avatarData);
            }
        } else {
            emit coreFile->fileTransferFinished(*file);
        }
        coreFile->removeFile(friendId, fileId);
        return;
    }

    if (file->fileKind == TOX_FILE_KIND_AVATAR) {
        file->avatarData.append(reinterpret_cast<const char*>(data), length);
    } else {
        file->file->write(reinterpret_cast<const char*>(data), length);
    }
    file->progress.addSample(file->progress.getBytesSent() + length);
    file->hashGenerator->addData(QByteArrayView(data));

    if (file->fileKind != TOX_FILE_KIND_AVATAR) {
        emit coreFile->fileTransferInfo(*file);
    }
}

void CoreFile::onConnectionStatusChanged(uint32_t friendId, Status::Status state)
{
    bool isOffline = state == Status::Status::Offline;
    // TODO: Actually resume broken file transfers
    // We need to:
    // - Start a new file transfer with the same 32byte file ID with toxcore
    // - Seek to the correct position again
    // - Update the fileNum in our ToxFile
    // - Update the users of our signals to check the 32byte tox file ID, not the uint32_t file_num
    // (fileId)
    ToxFile::FileStatus status = !isOffline ? ToxFile::TRANSMITTING : ToxFile::BROKEN;
    for (uint64_t key : fileMap.keys()) {
        if (key >> 32 != friendId)
            continue;
        fileMap[key].status = status;
        emit fileTransferBrokenUnbroken(fileMap[key], isOffline);
        removeFile(friendId, fileMap[key].fileNum);
    }
}
