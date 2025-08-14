/*
 *  SPDX-FileCopyrightText: 2016 Jouni Pentikäinen <joupent@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisTimeBasedItemModel.h"

#include <QPointer>
#include <kis_config.h>

#include "kis_animation_frame_cache.h"
#include "KisCanvasAnimationState.h"
#include "animation/KisFrameDisplayProxy.h"
#include "kis_signal_compressor_with_param.h"
#include "kis_image.h"
#include "kis_image_animation_interface.h"
#include "kis_time_span.h"
#include "KisAnimUtils.h"
#include "kis_keyframe_channel.h"
#include "kis_raster_keyframe_channel.h"
#include "kis_processing_applicator.h"
#include "KisImageBarrierLock.h"
#include "commands_new/kis_switch_current_time_command.h"
#include "kis_command_utils.h"
#include "KisPart.h"
#include "KisPlaybackEngine.h"
#include "kis_animation_cache_populator.h"

struct KisTimeBasedItemModel::Private
{
    Private()
        : animationPlayer(0)
        , document(nullptr)
        , numFramesOverride(0)
        , activeFrameIndex(0)
        , scrubInProgress(false)
        , scrubStartFrame(-1)
        , shouldReturnToPlay(false)
        , scrubHeaderMin(0)
        , scrubHeaderMax(0)
    {}

    KisImageWSP image;
    KisAnimationFrameCacheWSP framesCache;
    QPointer<KisCanvasAnimationState> animationPlayer;
    KisDocument* document;

    QVector<bool> cachedFrames;

    int numFramesOverride;
    int activeFrameIndex;

    bool scrubInProgress;
    int scrubStartFrame;
    bool shouldReturnToPlay;

    QScopedPointer<KisSignalCompressorWithParam<int>> scrubHeaderUpdateCompressor;
    int scrubHeaderMin;
    int scrubHeaderMax;

    int baseNumFrames() const {

        auto imageSP = image.toStrongRef();
        if (!imageSP) return 0;

        KisImageAnimationInterface *i = imageSP->animationInterface();
        if (!i) return 1;

        return i->totalLength();
    }

    int effectiveNumFrames() const {
        if (image.isNull()) return 0;

        return qMax(baseNumFrames(), numFramesOverride);
    }

    int framesPerSecond() {
        return image->animationInterface()->framerate();
    }

    bool withinClipRange(const int time) {
        if (!image) {
            return true;
        }

        KisTimeSpan clipRange = image->animationInterface()->documentPlaybackRange();
        return clipRange.contains(time);
    }
};

KisTimeBasedItemModel::KisTimeBasedItemModel(QObject *parent)
    : QAbstractTableModel(parent)
    , m_d(new Private())
{
    KisConfig cfg(true);

    using namespace std::placeholders;

    std::function<void (int)> scrubHorizHeaderUpdateCallback(
        std::bind(&KisTimeBasedItemModel::scrubHorizontalHeaderUpdate, this, _1));

    m_d->scrubHeaderUpdateCompressor.reset(
        new KisSignalCompressorWithParam<int>(100, scrubHorizHeaderUpdateCallback, KisSignalCompressor::FIRST_ACTIVE));
}

KisTimeBasedItemModel::~KisTimeBasedItemModel()
{}

void KisTimeBasedItemModel::setImage(KisImageWSP p_image)
{
    if (m_d->image == p_image ) {
        return;
    }

    beginResetModel();

    if (m_d->image) {
        //Disconnect old image..
        const KisImageAnimationInterface *ai = m_d->image->animationInterface();
        ai->disconnect(this);
    }

    m_d->image = p_image;
    m_d->numFramesOverride = m_d->effectiveNumFrames();

    if (m_d->image) {
        KisImageAnimationInterface *ai = m_d->image->animationInterface();

        connect(ai, SIGNAL(sigFramerateChanged()), this, SLOT(slotFramerateChanged()));
        connect(ai, SIGNAL(sigUiTimeChanged(int)), this, SLOT(slotCurrentTimeChanged(int)));
        connect(ai, SIGNAL(sigPlaybackRangeChanged()), this, SLOT(slotPlaybackRangeChanged()));
    }

    endResetModel();
}

void KisTimeBasedItemModel::setFrameCache(KisAnimationFrameCacheSP cache)
{
    if (KisAnimationFrameCacheSP(m_d->framesCache) == cache) return;

    if (m_d->framesCache) {
        m_d->framesCache->disconnect(this);
    }

    m_d->framesCache = cache;

    if (m_d->framesCache) {
        connect(m_d->framesCache, SIGNAL(changed()), SLOT(slotCacheChanged()));
    }
}

bool KisTimeBasedItemModel::isFrameCached(const int frame)
{
    return m_d->framesCache && m_d->framesCache->frameStatus(frame) == KisAnimationFrameCache::Cached;
}

void KisTimeBasedItemModel::setAnimationPlayer(KisCanvasAnimationState *player)
{
    if (m_d->animationPlayer == player) return;

    if (m_d->animationPlayer) {
        m_d->animationPlayer->disconnect(this);
    }

    m_d->animationPlayer = player;

    if (m_d->animationPlayer) {
        connect(m_d->animationPlayer, SIGNAL(sigPlaybackStateChanged(PlaybackState)), SLOT(slotPlaybackStateChanged(PlaybackState)));
        connect(m_d->animationPlayer, SIGNAL(sigFrameChanged()), SLOT(slotPlaybackFrameChanged()));

        const int frame = player ? player->displayProxy()->activeFrame() : m_d->image->animationInterface()->currentUITime();
        setHeaderData(frame, Qt::Horizontal, true, ActiveFrameRole);

        // only prioritize the cache, no seek operation to prevent audio from playing
        setHeaderData(frame, Qt::Horizontal, QVariant(int(SEEK_NONE)), ScrubToRole);
    }
}

void KisTimeBasedItemModel::setDocument(KisDocument *document)
{
    if (m_d->document == document) return;

    m_d->document = document;
}

KisDocument* KisTimeBasedItemModel::document() const
{
    return m_d->document;
}

void KisTimeBasedItemModel::setLastVisibleFrame(int time)
{
    const int growThreshold = m_d->effectiveNumFrames() - 1;
    const int growValue = time + 8;

    const int shrinkThreshold = m_d->effectiveNumFrames() - 3;
    const int shrinkValue = qMax(m_d->baseNumFrames(), qMin(growValue, shrinkThreshold));
    const bool canShrink = m_d->baseNumFrames() < m_d->effectiveNumFrames();

    if (time >= growThreshold) {
        beginInsertColumns(QModelIndex(), m_d->effectiveNumFrames(), growValue - 1);
        m_d->numFramesOverride = growValue;
        endInsertColumns();
    } else if (time < shrinkThreshold && canShrink) {
        beginRemoveColumns(QModelIndex(), shrinkValue, m_d->effectiveNumFrames() - 1);
        m_d->numFramesOverride = shrinkValue;
        endRemoveColumns();
    }
}

int KisTimeBasedItemModel::columnCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    return m_d->numFramesOverride;
}

QVariant KisTimeBasedItemModel::data(const QModelIndex &index, int role) const
{
    switch (role) {
        case ActiveFrameRole: {
            return index.column() == m_d->activeFrameIndex;
        }
        case CloneOfActiveFrame: {
            return cloneOfActiveFrame(index);
        }
        case CloneCount: {
            return cloneCount(index);
        }
        case WithinClipRange:
            return m_d->withinClipRange(index.column());
        }

    return QVariant();
}

bool KisTimeBasedItemModel::setData(const QModelIndex &index, const QVariant &value, int role)
{
    if (!index.isValid()) return false;

    switch (role) {
        case ActiveFrameRole:
            setHeaderData(index.column(), Qt::Horizontal, value, role);
            break;
        case ScrubToRole:
            setHeaderData(index.column(), Qt::Horizontal, value, role);
            break;
    }

    return false;
}

QVariant KisTimeBasedItemModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation == Qt::Horizontal) {
        switch (role) {
        case ActiveFrameRole:
            return section == m_d->activeFrameIndex;
        case FrameCachedRole:
            return m_d->cachedFrames.size() > section ? m_d->cachedFrames[section] : false;
        case FramesPerSecondRole:
            return m_d->framesPerSecond();
        case WithinClipRange:
            return m_d->withinClipRange(section);
        }
    }

    return QVariant();
}

bool KisTimeBasedItemModel::setHeaderData(int section, Qt::Orientation orientation, const QVariant &value, int role)
{
    auto prioritizeCache = [this](int frame){
        if (m_d->image) {
            if(!isFrameCached(frame)) {
                KisPart::instance()->prioritizeFrameForCache(m_d->image, frame);
            }
        }
    };


    if (orientation == Qt::Horizontal) {
        switch (role) {
        case ActiveFrameRole:
            if (value.toBool() &&
                section != m_d->activeFrameIndex) {

                int prevFrame = m_d->activeFrameIndex;
                m_d->activeFrameIndex = section;

                /**
                 * Optimization Hack Alert:
                 *
                 * ideally, we should Q_EMIT all four signals, but... The
                 * point is this code is used in a tight loop during
                 * playback, so it should run as fast as possible. To tell
                 * the story short, commenting out these three lines makes
                 * playback run 15% faster ;)
                 */

                if (m_d->scrubInProgress) {
                    Q_EMIT dataChanged(this->index(0, m_d->activeFrameIndex), this->index(rowCount() - 1, m_d->activeFrameIndex));

                    /*
                     * In order to try to correct rendering issues while preserving performance, we will
                     * defer updates just long enough that visual artifacts aren't majorly noticeable.
                     * By using a signal compressor, we're going to update the range of columns between
                     * min / max. That min max is reset every time the update occurs. This should fix
                     * rendering issues to a configurable framerate.
                     */
                    m_d->scrubHeaderMin = qMin(m_d->activeFrameIndex, m_d->scrubHeaderMin);
                    m_d->scrubHeaderMax = qMax(m_d->activeFrameIndex, m_d->scrubHeaderMax);
                    m_d->scrubHeaderUpdateCompressor->start(m_d->activeFrameIndex);

                    // vvvvvvvvvvvvvvvvvvvvv Read above comment.. This fixes all timeline rendering issues, but at what cost???
                    //emit dataChanged(this->index(0, prevFrame), this->index(rowCount() - 1, prevFrame));
                    //emit headerDataChanged (Qt::Horizontal, m_d->activeFrameIndex, m_d->activeFrameIndex);
                    //emit headerDataChanged (Qt::Horizontal, prevFrame, prevFrame);
                } else {
                    Q_EMIT dataChanged(this->index(0, prevFrame), this->index(rowCount() - 1, prevFrame));
                    Q_EMIT dataChanged(this->index(0, m_d->activeFrameIndex), this->index(rowCount() - 1, m_d->activeFrameIndex));
                    Q_EMIT headerDataChanged (Qt::Horizontal, prevFrame, prevFrame);
                    Q_EMIT headerDataChanged (Qt::Horizontal, m_d->activeFrameIndex, m_d->activeFrameIndex);
                }
            }
            break;
        case ScrubToRole:
            SeekOptionFlags seekFlags = SeekOptionFlags(value.toInt());
            prioritizeCache(m_d->activeFrameIndex);
            if (!m_d->image->hasUpdatesRunning()) {
                KisPart::instance()->playbackEngine()->seek(m_d->activeFrameIndex, seekFlags);
            }
            break;
        }
    }

    return false;
}

void KisTimeBasedItemModel::scrubHorizontalHeaderUpdate(int activeColumn)
{
    Q_EMIT headerDataChanged (Qt::Horizontal, m_d->scrubHeaderMin, m_d->scrubHeaderMax);
    m_d->scrubHeaderMin = activeColumn;
    m_d->scrubHeaderMax = activeColumn;
}

bool KisTimeBasedItemModel::removeFrames(const QModelIndexList &indexes)
{
    KisAnimUtils::FrameItemList frameItems;

    {
        KisImageBarrierLock locker(m_d->image);

        Q_FOREACH (const QModelIndex &index, indexes) {
            int time = index.column();
            Q_FOREACH(KisKeyframeChannel *channel, channelsAt(index)) {
                if (channel->keyframeAt(time)) {
                    frameItems << KisAnimUtils::FrameItem(channel->node(), channel->id(), index.column());
                }
            }
        }
    }

    if (frameItems.isEmpty()) return false;

    KisAnimUtils::removeKeyframes(m_d->image, frameItems);

    return true;
}

KUndo2Command* KisTimeBasedItemModel::createOffsetFramesCommand(QModelIndexList srcIndexes,
                                                                const QPoint &offset,
                                                                bool copyFrames,
                                                                bool moveEmptyFrames,
                                                                KUndo2Command *parentCommand)
{
    if (srcIndexes.isEmpty()) return 0;
    if (offset.isNull()) return 0;

    KisAnimUtils::sortPointsForSafeMove(&srcIndexes, offset);

    KisAnimUtils::FrameItemList srcFrameItems;
    KisAnimUtils::FrameItemList dstFrameItems;

    Q_FOREACH (const QModelIndex &srcIndex, srcIndexes) {
        QModelIndex dstIndex = index(
                srcIndex.row() + offset.y(),
                srcIndex.column() + offset.x());

        KisNodeSP srcNode = nodeAt(srcIndex);
        KisNodeSP dstNode = nodeAt(dstIndex);
        if (!srcNode || !dstNode) return 0;

        Q_FOREACH(KisKeyframeChannel *channel, channelsAt(srcIndex)) {
            if (moveEmptyFrames || channel->keyframeAt(srcIndex.column())) {
                srcFrameItems << KisAnimUtils::FrameItem(srcNode, channel->id(), srcIndex.column());
                dstFrameItems << KisAnimUtils::FrameItem(dstNode, channel->id(), dstIndex.column());
            }
        }
    }

    KIS_SAFE_ASSERT_RECOVER_RETURN_VALUE(srcFrameItems.size() == dstFrameItems.size(), 0);
    if (srcFrameItems.isEmpty()) return 0;

    return
        KisAnimUtils::createMoveKeyframesCommand(srcFrameItems,
                                                      dstFrameItems,
                                                      copyFrames,
                                                      moveEmptyFrames,
                                                      parentCommand);
}

bool KisTimeBasedItemModel::removeFramesAndOffset(QModelIndexList indicesToRemove)
{
    if (indicesToRemove.isEmpty()) return true;

    std::sort(indicesToRemove.begin(), indicesToRemove.end(),
              [] (const QModelIndex &lhs, const QModelIndex &rhs) {
                  return lhs.column() > rhs.column();
              });

    const int minColumn = indicesToRemove.last().column();

    KUndo2Command *parentCommand = new KUndo2Command(kundo2_i18np("Remove frame and shift", "Remove %1 frames and shift", indicesToRemove.size()));

    {
        KisImageBarrierLock locker(m_d->image);

        Q_FOREACH (const QModelIndex &index, indicesToRemove) {
            QModelIndexList indicesToOffset;
            for (int column = index.column() + 1; column < columnCount(); column++) {
                indicesToOffset << this->index(index.row(), column);
            }
            createOffsetFramesCommand(indicesToOffset, QPoint(-1, 0), false, true, parentCommand);
        }

        const int oldTime = m_d->image->animationInterface()->currentUITime();
        const int newTime = minColumn;

        new KisSwitchCurrentTimeCommand(m_d->image->animationInterface(),
                                        oldTime,
                                        newTime,
                                        parentCommand);
    }

    KisProcessingApplicator::runSingleCommandStroke(m_d->image, parentCommand,
                                                    KisStrokeJobData::BARRIER,
                                                    KisStrokeJobData::EXCLUSIVE);
    return true;
}

bool KisTimeBasedItemModel::mirrorFrames(QModelIndexList indexes)
{
    QScopedPointer<KUndo2Command> parentCommand(new KUndo2Command(kundo2_i18n("Mirror Frames")));

    {
        KisImageBarrierLock locker(m_d->image);

        QMap<int, QModelIndexList> rowsList;

        Q_FOREACH (const QModelIndex &index, indexes) {
            rowsList[index.row()].append(index);
        }


        Q_FOREACH (int row, rowsList.keys()) {
            QModelIndexList &list = rowsList[row];

            KIS_SAFE_ASSERT_RECOVER_RETURN_VALUE(!list.isEmpty(), false);

            std::sort(list.begin(), list.end(),
                [] (const QModelIndex &lhs, const QModelIndex &rhs) {
                    return lhs.column() < rhs.column();
                });

            auto srcIt = list.begin();
            auto dstIt = list.end();

            KIS_SAFE_ASSERT_RECOVER_RETURN_VALUE(srcIt != dstIt, false);
            --dstIt;

            QList<KisKeyframeChannel*> channels = channelsAt(*srcIt).values();

            while (srcIt < dstIt) {
                Q_FOREACH (KisKeyframeChannel *channel, channels) {
                    if (channel->keyframeAt(srcIt->column()) && channel->keyframeAt(dstIt->column())) {

                        channel->swapKeyframes(srcIt->column(),
                                               dstIt->column(),
                                               parentCommand.data());
                    }
                    else if (channel->keyframeAt(srcIt->column())) {

                        channel->insertKeyframe(dstIt->column(),
                                                channel->keyframeAt(srcIt->column()),
                                                parentCommand.data());

                        channel->removeKeyframe(srcIt->column(),
                                                parentCommand.data());
                    }
                    else if (channel->keyframeAt(dstIt->column())) {

                        channel->insertKeyframe(srcIt->column(),
                                                channel->keyframeAt(dstIt->column()),
                                                parentCommand.data());

                        channel->removeKeyframe(dstIt->column(),
                                                parentCommand.data());
                    }
                }

                srcIt++;
                dstIt--;
            }
        }
    }

    KisProcessingApplicator::runSingleCommandStroke(m_d->image,
                                                    parentCommand.take(),
                                                    KisStrokeJobData::BARRIER,
                                                    KisStrokeJobData::EXCLUSIVE);
    return true;
}

void KisTimeBasedItemModel::setScrubState(bool p_state)
{
    if (!m_d->animationPlayer) {
        return;
    }

    if (m_d->scrubInProgress != p_state) {
        m_d->scrubInProgress = p_state;

        if (m_d->scrubInProgress == true) {
            m_d->scrubStartFrame = m_d->activeFrameIndex;

            if (m_d->animationPlayer->playbackState() == PLAYING) {
                m_d->shouldReturnToPlay = true;
                m_d->animationPlayer->setPlaybackState(PAUSED);
            }

        } else {
            if (m_d->shouldReturnToPlay) {
                m_d->animationPlayer->setPlaybackState(PLAYING);
            }

            m_d->scrubStartFrame = -1;
            m_d->shouldReturnToPlay = false;
        }
    }
}

bool KisTimeBasedItemModel::isScrubbing()
{
    return m_d->scrubInProgress;
}


void KisTimeBasedItemModel::slotCurrentTimeChanged(int time)
{
    if (time != m_d->activeFrameIndex) {
        setHeaderData(time, Qt::Horizontal, true, ActiveFrameRole);
    }
}

void KisTimeBasedItemModel::slotFramerateChanged()
{
    Q_EMIT headerDataChanged(Qt::Horizontal, 0, columnCount() - 1);
}

void KisTimeBasedItemModel::slotPlaybackRangeChanged()
{
    if (m_d->image && m_d->image->animationInterface() ) {
        const KisImageAnimationInterface* const interface = m_d->image->animationInterface();
        const int lastFrame = interface->activePlaybackRange().end();
        if (lastFrame > m_d->numFramesOverride) {
            beginInsertColumns(QModelIndex(), m_d->numFramesOverride, interface->activePlaybackRange().end());
            m_d->numFramesOverride = interface->activePlaybackRange().end();
            endInsertColumns();
        }

        dataChanged(index(0,0), index(rowCount(), columnCount()));
    }
}

void KisTimeBasedItemModel::slotCacheChanged()
{
    const int numFrames = columnCount();
    m_d->cachedFrames.resize(numFrames);

    for (int i = 0; i < numFrames; i++) {
        m_d->cachedFrames[i] =
            m_d->framesCache->frameStatus(i) == KisAnimationFrameCache::Cached;
    }

    Q_EMIT headerDataChanged(Qt::Horizontal, 0, numFrames);
}


void KisTimeBasedItemModel::slotPlaybackFrameChanged()
{
    if (m_d->animationPlayer->playbackState() != PlaybackState::PLAYING) return;
    setHeaderData(m_d->animationPlayer->displayProxy()->activeFrame(), Qt::Horizontal, true, ActiveFrameRole);
}

void KisTimeBasedItemModel::slotPlaybackStateChanged(PlaybackState p_state)
{
    if (p_state == PlaybackState::STOPPED) {
        setHeaderData(m_d->image->animationInterface()->currentUITime(), Qt::Horizontal, true, ActiveFrameRole);
    }
}

void KisTimeBasedItemModel::setPlaybackRange(const KisTimeSpan &range)
{
    if (m_d->image.isNull()) return;

    KisImageAnimationInterface *i = m_d->image->animationInterface();
    i->setActivePlaybackRange(range);
}

bool KisTimeBasedItemModel::isPlaybackActive() const
{
    return m_d->animationPlayer && m_d->animationPlayer->playbackState() == PlaybackState::PLAYING;
}

bool KisTimeBasedItemModel::isPlaybackPaused() const
{
    return m_d->animationPlayer && m_d->animationPlayer->playbackState() == PlaybackState::PAUSED;
}

void KisTimeBasedItemModel::stopPlayback() const {
    KisPart::instance()->playbackEngine()->stop();
}

int KisTimeBasedItemModel::currentTime() const
{
    return m_d->image->animationInterface()->currentUITime();
}

bool KisTimeBasedItemModel::cloneOfActiveFrame(const QModelIndex &index) const {
    KisRasterKeyframeChannel *rasterChan = dynamic_cast<KisRasterKeyframeChannel*>(channelByID(index, KisKeyframeChannel::Raster.id()));
    if (!rasterChan) return false;

    const int activeKeyframeTime = rasterChan->activeKeyframeTime(m_d->activeFrameIndex);
    return rasterChan->areClones(activeKeyframeTime, index.column());
}

int KisTimeBasedItemModel::cloneCount(const QModelIndex &index) const {
    KisRasterKeyframeChannel *rasterChan = dynamic_cast<KisRasterKeyframeChannel*>(channelByID(index, KisKeyframeChannel::Raster.id()));

    if (!rasterChan) {
        return 0;
    }
    return rasterChan->clonesOf(index.column()).count();
}

KisImageWSP KisTimeBasedItemModel::image() const
{
    return m_d->image;
}
