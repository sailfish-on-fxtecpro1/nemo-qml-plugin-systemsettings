/*
 * Copyright (C) 2016 Jolla Ltd. <andrew.den.exter@jolla.com>
 *
 * You may use this file under the terms of the BSD license as follows:
 *
 * "Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in
 *     the documentation and/or other materials provided with the
 *     distribution.
 *   * Neither the name of Nemo Mobile nor the names of its contributors
 *     may be used to endorse or promote products derived from this
 *     software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE."
 */

#include "partitionmanager_p.h"
#include "udisks2monitor_p.h"

#include <QFile>
#include <QRegularExpression>

#include <blkid/blkid.h>
#include <linux/limits.h>
#include <mntent.h>
#include <sys/statvfs.h>

static const auto userName = QString(qgetenv("USER"));
static const auto externalMountPath = QString("/run/media/%1/").arg(userName);

PartitionManagerPrivate *PartitionManagerPrivate::sharedInstance = nullptr;

PartitionManagerPrivate::PartitionManagerPrivate()
{
    Q_ASSERT(!sharedInstance);

    sharedInstance = this;
    m_udisksMonitor.reset(new UDisks2::Monitor(this));

    QExplicitlySharedDataPointer<PartitionPrivate> root(new PartitionPrivate(this));
    root->storageType = Partition::System;
    root->mountPath = QStringLiteral("/");

    m_partitions.append(root);

    QExplicitlySharedDataPointer<PartitionPrivate> home(new PartitionPrivate(this));
    home->storageType = Partition::User;
    home->mountPath = QStringLiteral("/home");

    m_partitions.append(home);

    refresh();

    // Remove any prospective internal partitions that aren't mounted.
    int internalPartitionCount = 0;
    for (Partitions::iterator it = m_partitions.begin(); it != m_partitions.end();) {
        auto partition = *it;

        if (partition->storageType & Partition::Internal) {
            if (partition->status == Partition::Mounted) {
                internalPartitionCount += 1;
            } else {
                it = m_partitions.erase(it);
                continue;
            }
        }

        ++it;
    }

    // Check that /home is not actually the same device as /
    if (home->status == Partition::Mounted && root->status == Partition::Mounted &&
        home->devicePath == root->devicePath) {
        m_partitions.erase(m_partitions.begin() + 1);
        --internalPartitionCount;
    }

    if (internalPartitionCount == 1) {
        root->storageType = Partition::Mass;
    }

    if (root->status == Partition::Mounted) {
        m_root = Partition(QExplicitlySharedDataPointer<PartitionPrivate>(root));
    }
}

PartitionManagerPrivate::~PartitionManagerPrivate()
{
    sharedInstance = nullptr;

    for (auto partition : m_partitions) {
        partition->manager = nullptr;
    }
}

PartitionManagerPrivate *PartitionManagerPrivate::instance()
{
    return sharedInstance ? sharedInstance : new PartitionManagerPrivate;
}

Partition PartitionManagerPrivate::root() const
{
    return m_root;
}

QVector<Partition> PartitionManagerPrivate::partitions(const Partition::StorageTypes types) const
{
    QVector<Partition> partitions;

    for (const auto partition : m_partitions) {
        if (partition->storageType & types) {
            if ((types & Partition::ExcludeParents)
                    && !partitions.isEmpty()
                    && partitions.last().d->isParent(partition)) {
                partitions.last() = Partition(partition);
            } else {
                partitions.append(Partition(partition));
            }
        }
    }

    return partitions;
}

void PartitionManagerPrivate::refresh()
{
    int index;
    for (index = 0; index < m_partitions.count(); ++index) {
        if (m_partitions.at(index)->storageType == Partition::External) {
            break;
        }
    }

    Partitions addedPartitions;
    Partitions changedPartitions;

    QFile partitionFile(QStringLiteral("/proc/partitions"));
    if (partitionFile.open(QIODevice::ReadOnly)) {
        // Read headers.
        partitionFile.readLine();
        partitionFile.readLine();

        static const QRegularExpression whitespace(QStringLiteral("\\s+"));
        static const QRegularExpression externalMedia(QString("^%1$").arg(externalDevice));
        static const QRegularExpression deviceRoot(QStringLiteral("^mmcblk\\d+$"));

        while (!partitionFile.atEnd()) {
            QStringList line = QString::fromUtf8(partitionFile.readLine()).split(whitespace, QString::SkipEmptyParts);

            if (line.count() != 4) {
                continue;
            }

            const QString deviceName = line.at(3);

            if (!externalMedia.match(deviceName).hasMatch()) {
                continue;
            }

            const auto partition = [&]() {
                for (int i = index; i < m_partitions.count(); ++i) {
                    const auto partition = m_partitions.at(i);
                    if (partition->deviceName == deviceName) {
                        if (index != i) {
                            m_partitions.removeAt(i);
                            m_partitions.insert(index, partition);
                        }

                        changedPartitions.append(partition);

                        return partition;
                    }
                }
                QExplicitlySharedDataPointer<PartitionPrivate> partition(new PartitionPrivate(this));
                partition->storageType = Partition::External;
                partition->deviceName = deviceName;
                partition->devicePath = QStringLiteral("/dev/") + deviceName;
                partition->deviceRoot = deviceRoot.match(deviceName).hasMatch();

                m_partitions.insert(index, partition);
                addedPartitions.append(partition);

                return partition;
            }();

            partition->bytesTotal = line.at(2).toInt() * 1024;

            ++index;
        }
    }

    const auto removedPartitions = m_partitions.mid(index);
    m_partitions.resize(index);

    refresh(m_partitions, changedPartitions);

    for (const auto partition : removedPartitions) {
        emit partitionRemoved(Partition(partition));
    }

    for (const auto partition : addedPartitions) {
        emit partitionAdded(Partition(partition));
    }

    for (const auto partition : changedPartitions) {
        emit partitionChanged(Partition(partition));
    }
}

void PartitionManagerPrivate::refresh(PartitionPrivate *partition)
{
    refresh(Partitions() << QExplicitlySharedDataPointer<PartitionPrivate>(partition), Partitions() << QExplicitlySharedDataPointer<PartitionPrivate>(partition));

    emit partitionChanged(Partition(QExplicitlySharedDataPointer<PartitionPrivate>(partition)));
}

void PartitionManagerPrivate::refresh(const Partitions &partitions, Partitions &changedPartitions)
{
    for (auto partition : partitions) {
        // Reset properties to the unmounted defaults.  If the partition is mounted these will be restored
        // by the refresh.
        partition->status = partition->activeState == QStringLiteral("activating")
                ? Partition::Mounting
                : Partition::Unmounted;
        partition->bytesFree = 0;
        partition->bytesAvailable = 0;
        partition->canMount = false;
        partition->readOnly = true;
        partition->filesystemType.clear();
    }

    FILE *mtab = setmntent("/etc/mtab", "r");
    mntent mountEntry;
    char buffer[3 * PATH_MAX];

    while (getmntent_r(mtab, &mountEntry, buffer, sizeof(buffer))) {
        if (!mountEntry.mnt_fsname || !mountEntry.mnt_dir) {
            continue;
        }

        const QString mountPath = QString::fromUtf8(mountEntry.mnt_dir);
        const QString devicePath = QString::fromUtf8(mountEntry.mnt_fsname);

        for (auto partition : partitions) {
            if ((partition->status == Partition::Mounted || partition->status == Partition::Mounting)
                    && (partition->storageType != Partition::External || partition->mountPath.startsWith(externalMountPath))) {
                continue;
            }

            if (((partition->storageType & Partition::Internal)
                            && partition->mountPath == mountPath
                            && devicePath.startsWith(QLatin1Char('/')))
                        || (partition->storageType == Partition::External
                            && partition->devicePath == devicePath)) {
                partition->mountPath = mountPath;
                partition->devicePath = devicePath;
                partition->filesystemType = QString::fromUtf8(mountEntry.mnt_type);
                partition->status = partition->activeState == QStringLiteral("deactivating")
                        ? Partition::Unmounting
                        : Partition::Mounted;
                partition->canMount = true;
            }
        }
    }

    endmntent(mtab);

    blkid_cache cache = nullptr;

    // Query filesystems supported by this device
    // Note this will only find filesystems supported either directly by the
    // kernel, or by modules already loaded.
    QStringList supportedFs;
    QFile filesystems(QStringLiteral("/proc/filesystems"));
    if (filesystems.open(QIODevice::ReadOnly)) {
        QString line = filesystems.readLine();
        while (line.length() > 0) {
            supportedFs << line.trimmed().split('\t').last();
            line = filesystems.readLine();
        }
    }
    
    for (auto partition : partitions) {
        if (partition->status == Partition::Mounted) {
            struct statvfs64 stat;
            if (::statvfs64(partition->mountPath.toUtf8().constData(), &stat) == 0) {
                partition->bytesTotal = stat.f_blocks * stat.f_frsize;
                qint64 bytesFree = stat.f_bfree * stat.f_frsize;
                qint64 bytesAvailable = stat.f_bavail * stat.f_frsize;
                partition->readOnly = (stat.f_flag & ST_RDONLY) != 0;

                if (partition->bytesFree != bytesFree || partition->bytesAvailable != bytesAvailable) {
                    if (!changedPartitions.contains(partition)) {
                        changedPartitions.append(partition);
                    }
                }
                partition->bytesFree = bytesFree;
                partition->bytesAvailable = bytesAvailable;
            }
        } else if (partition->storageType == Partition::External) {
            // Presume the file system can be mounted, unless we can confirm otherwise.
            partition->canMount = true;

            // If an external partition is unmounted, query the uuid to get the prospective mount path.
            if (!cache && blkid_get_cache(&cache, nullptr) < 0) {
                qWarning("Failed to load blkid cache");
                continue;
            }

            // Directly probing the device would be better but requires root permissions.
            if (char * const uuid = blkid_get_tag_value(
                        cache, "UUID", partition->devicePath.toUtf8().constData())) {
                partition->mountPath = externalMountPath + QString::fromUtf8(uuid);

                ::free(uuid);
            }

            if (char * const type = blkid_get_tag_value(
                        cache, "TYPE", partition->devicePath.toUtf8().constData())) {
                partition->filesystemType = QString::fromUtf8(type);
                partition->canMount = !partition->filesystemType.isEmpty() && supportedFs.contains(partition->filesystemType);

                ::free(type);
            }
        }
    }
}

PartitionManager::PartitionManager(QObject *parent)
    : QObject(parent)
    , d(PartitionManagerPrivate::instance())
{
    connect(d.data(), &PartitionManagerPrivate::partitionChanged, this, &PartitionManager::partitionChanged);
    connect(d.data(), &PartitionManagerPrivate::partitionAdded, this, &PartitionManager::partitionAdded);
    connect(d.data(), &PartitionManagerPrivate::partitionRemoved, this, &PartitionManager::partitionRemoved);
}

PartitionManager::~PartitionManager()
{
}

Partition PartitionManager::root() const
{
    return d->root();
}

QVector<Partition> PartitionManager::partitions(Partition::StorageTypes types) const
{
    return d->partitions(types);
}

void PartitionManager::refresh()
{
    d->refresh();
}
