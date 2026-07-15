/*
    GMCA — music detail views (issue #11). See tab/media_music.hpp.
*/

#include "tab/media_music.hpp"
#include "api/backend.hpp"
#include "utils/config.hpp"
#include "utils/misc.hpp"
#include "utils/image.hpp"
#include "view/recycling_grid.hpp"
#include "view/video_card.hpp"
#include "view/video_source.hpp"
#include "view/music_now_playing.hpp"
#include "view/icon_button.hpp"
#include <algorithm>

using namespace brls::literals;  // for _i18n

// ---- Album track list --------------------------------------------------------

/// Album header cell (cover + info + Play): first flow cell, scrolls with the
/// tracks. Not focusable itself; navigation reaches the Play button.
class AlbumHeaderCell : public RecyclingGridItem {
public:
    AlbumHeaderCell() {
        this->inflateFromXMLRes("xml/view/album_header.xml");
        this->setFocusable(false);
        this->btnPlay->registerClickAction([this](...) {
            if (this->onPlay) this->onPlay();
            return true;
        });
    }

    void setItem(const media::Item& album, size_t trackCount) {
        this->cover->clear();
        const std::string& art = album.thumb.empty() ? album.parentThumb : album.thumb;
        if (!art.empty()) Image::load(this->cover, art, 225);
        this->labelTitle->setText(album.title);

        std::string meta = album.parentTitle;  // album artist
        if (album.year > 0) meta += (meta.empty() ? "" : "  ·  ") + std::to_string(album.year);
        int64_t n = album.leafCount > 0 ? album.leafCount : (int64_t)trackCount;
        if (n > 0) {
            std::string tracks = fmt::format("{} {}", n, n > 1 ? "main/music/tracks"_i18n : "main/music/track"_i18n);
            meta += (meta.empty() ? "" : "  ·  ") + tracks;
        }
        this->labelMeta->setText(meta);
        this->labelMeta->setVisibility(meta.empty() ? brls::Visibility::GONE : brls::Visibility::VISIBLE);
    }

    void prepareForReuse() override { this->cover->clear(); }  // transparent -> note placeholder
    void cacheForReuse() override { Image::cancel(this->cover); }

    std::function<void()> onPlay = nullptr;

private:
    BRLS_BIND(brls::Image, cover, "album/image/cover");
    BRLS_BIND(brls::Label, labelTitle, "album/label/title");
    BRLS_BIND(brls::Label, labelMeta, "album/label/meta");
    BRLS_BIND(IconButton, btnPlay, "album/play");
};

/// Track row (BaseCardCell for the shared focus/ticker/long-press machinery).
class TrackCell : public BaseCardCell {
public:
    TrackCell() { this->inflateFromXMLRes("xml/view/track_row.xml"); }

    static TrackCell* create() { return new TrackCell(); }

    BRLS_BIND(brls::Label, labelIndex, "track/row/index");
};

/// Album view source: index 0 = header, 1..N = tracks. Fixed heights (no yoga
/// measuring), like SeasonEpisodesDataSource.
class AlbumTracksDataSource : public RecyclingGridDataSource {
public:
    static constexpr float HEADER_HEIGHT = 255;  // cover 225 + air 30
    static constexpr float ROW_HEIGHT = 56;      // 46 row + padding 2x5

    AlbumTracksDataSource(const media::Item& album, const std::vector<media::Item>& tracks)
        : album(album), list(tracks) {}

    size_t getItemCount() override { return this->list.size() + 1; }

    float heightForRow(brls::View* recycler, size_t index) override {
        return index == 0 ? HEADER_HEIGHT : ROW_HEIGHT;
    }

    RecyclingGridItem* cellForRow(RecyclingView* recycler, size_t index) override {
        if (index == 0) {
            auto* header = dynamic_cast<AlbumHeaderCell*>(recycler->dequeueReusableCell("Header"));
            header->setItem(this->album, this->list.size());
            header->onPlay = [this]() { MusicNowPlaying::present(this->list, 0, false); };
            return header;
        }

        auto* cell = dynamic_cast<TrackCell*>(recycler->dequeueReusableCell("Cell"));
        auto& item = this->list.at(index - 1);
        cell->setId(item.ratingKey);

        cell->picture->clear();
        std::string art = !item.thumb.empty() ? item.thumb : (album.thumb.empty() ? item.parentThumb : album.thumb);
        if (!art.empty()) Image::load(cell->picture, art, 92);

        cell->labelIndex->setText(item.index > 0 ? std::to_string(item.index) : "");
        cell->labelTitle->setText(item.title);
        cell->labelExt->setText(item.duration > 0 ? misc::sec2Time(item.duration / 1000) : "");
        return cell;
    }

    void onItemSelected(brls::Box* recycler, size_t index) override {
        if (index == 0) return;  // header acts through its Play button
        MusicNowPlaying::present(this->list, index - 1, false);
    }

    void clearData() override { this->list.clear(); }

private:
    media::Item album;
    std::vector<media::Item> list;
};

// ---- Album loading skeleton ---------------------------------------------------
// The generic RecyclingGrid skeleton (uniform full-width blocks, see
// SkeletonCell::draw) does not resemble this screen's actual layout: a big
// square cover + a couple of text lines + a button on the first row, then
// compact track rows. Mirror AlbumHeaderCell/TrackCell's shapes instead so the
// loading flash reads as "this album is loading", not as a random grid.

/// Header skeleton: cover square (album_header.xml: 225x225, radius 10) +
/// title/meta bars + a pill-shaped button bar, centered against the cover
/// like the real header's column (justifyContent="center").
class AlbumHeaderSkeletonCell : public SkeletonCell {
public:
    static RecyclingGridItem* create() { return new AlbumHeaderSkeletonCell(); }

    void draw(NVGcontext* vg, float x, float y, float width, float height, brls::Style style,
        brls::FrameContext* ctx) override {
        NVGpaint paint = this->shimmerPaint(vg, x, y, width, height);

        float cover = 225;
        bar(vg, paint, x, y, cover, cover, 10);

        // right column (marginLeft 24), like album_header.xml
        float colX = x + cover + 24;
        float colW = std::max(width - cover - 24, 40.0f);

        float titleH = 24, metaH = 14, btnH = 44;
        float contentH = titleH + 8 /* labelMeta marginTop */ + metaH + 14 /* button box marginTop */ + btnH;
        float top = y + (cover - contentH) / 2;

        bar(vg, paint, colX, top, std::min(colW, 400.0f) * 0.55f, titleH, 8);
        top += titleH + 8;
        bar(vg, paint, colX, top, std::min(colW, 400.0f) * 0.32f, metaH, 6);
        top += metaH + 14;
        bar(vg, paint, colX, top, std::min(colW, 180.0f), btnH, btnH / 2);
    }
};

/// Track row skeleton: small square cover (track_row.xml: 46x46, radius 8,
/// padding 5 -> fills ROW_HEIGHT exactly) + a title bar + a trailing duration
/// bar, mirroring [cover][title ....][duration].
class AlbumTrackSkeletonCell : public SkeletonCell {
public:
    static RecyclingGridItem* create() { return new AlbumTrackSkeletonCell(); }

    void draw(NVGcontext* vg, float x, float y, float width, float height, brls::Style style,
        brls::FrameContext* ctx) override {
        NVGpaint paint = this->shimmerPaint(vg, x, y, width, height);

        float pad = 5, cover = 46;
        bar(vg, paint, x + pad, y + pad, cover, cover, 8);

        // title bar, past where the index column sits (marginLeft 14 + width 40 + marginLeft 4)
        float titleX = x + pad + cover + 14 + 40 + 4;
        float durationW = 40;
        float durationX = x + width - pad - durationW;
        float titleW = std::max((durationX - 10 - titleX) * 0.55f, 0.0f);
        bar(vg, paint, titleX, y + height / 2 - 8, titleW, 16, 6);

        bar(vg, paint, durationX, y + height / 2 - 6, durationW, 12, 5);
    }
};

/// Loading placeholder for MediaAlbum: header skeleton + a handful of row
/// skeletons, same fixed heights as AlbumTracksDataSource so scrolling and
/// layout behave identically once the real data source replaces this one.
class AlbumSkeletonDataSource : public RecyclingGridDataSource {
public:
    static constexpr size_t ROWS = 8;

    size_t getItemCount() override { return ROWS + 1; }

    float heightForRow(brls::View* recycler, size_t index) override {
        return index == 0 ? AlbumTracksDataSource::HEADER_HEIGHT : AlbumTracksDataSource::ROW_HEIGHT;
    }

    RecyclingGridItem* cellForRow(RecyclingView* recycler, size_t index) override {
        return recycler->dequeueReusableCell(index == 0 ? "HeaderSkeleton" : "RowSkeleton");
    }

    void clearData() override {}
};

// ---- MediaAlbum --------------------------------------------------------------

MediaAlbum::MediaAlbum(const media::Item& item) : album(item) {
    this->inflateFromXMLRes("xml/tabs/album.xml");
    this->recycler->registerCell("Header", []() { return new AlbumHeaderCell(); });
    this->recycler->registerCell("Cell", []() { return new TrackCell(); });
    this->recycler->registerCell("HeaderSkeleton", AlbumHeaderSkeletonCell::create);
    this->recycler->registerCell("RowSkeleton", AlbumTrackSkeletonCell::create);
    this->recycler->setDataSource(new AlbumSkeletonDataSource());
    this->doRequest();
}

void MediaAlbum::doRequest() {
    ASYNC_RETAIN
    AppConfig::instance().backend().getChildren(
        this->album.ratingKey,
        [ASYNC_TOKEN](const media::Container<media::Item>& r) {
            ASYNC_RELEASE
            this->recycler->setDataSource(new AlbumTracksDataSource(this->album, r.Items));
        },
        [ASYNC_TOKEN](const std::string& ex) {
            ASYNC_RELEASE
            this->recycler->setError(ex);
        });
}

// ---- MediaArtist -------------------------------------------------------------

MediaArtist::MediaArtist(const media::Item& item) : artist(item) {
    this->inflateFromXMLRes("xml/tabs/media.xml");
    this->recycler->itemImageRatio = 1.0f;  // square album covers
    this->recycler->registerCell("Cell", VideoCardCell::create);

    brls::View* header = brls::View::createFromXMLResource("view/artist_header.xml");
    this->labelTitle = dynamic_cast<brls::Label*>(header->getView("grid/header/title"));
    this->labelMeta = dynamic_cast<brls::Label*>(header->getView("grid/header/meta"));
    if (brls::View* btn = header->getView("artist/shuffle"))
        btn->registerClickAction([this](...) {
            this->shufflePlay();
            return true;
        });
    this->recycler->setHeaderView(header, 150);
    if (this->labelTitle) this->labelTitle->setText(item.title);

    this->doRequest();
}

void MediaArtist::shufflePlay() {
    ASYNC_RETAIN
    AppConfig::instance().backend().getArtistTracks(
        this->artist.ratingKey,
        [ASYNC_TOKEN](const media::Container<media::Item>& r) {
            ASYNC_RELEASE
            if (!r.Items.empty()) MusicNowPlaying::present(r.Items, 0, true);  // shuffle
        },
        [ASYNC_TOKEN](const std::string& ex) {
            ASYNC_RELEASE
            brls::Application::notify(ex);
        });
}

void MediaArtist::doRequest() {
    ASYNC_RETAIN
    AppConfig::instance().backend().getArtistAlbums(
        this->artist.ratingKey,
        [ASYNC_TOKEN](const media::Container<media::Item>& r) {
            ASYNC_RELEASE
            if (this->labelMeta) {
                this->labelMeta->setText(fmt::format("{} {}", r.Items.size(),
                    r.Items.size() > 1 ? "main/music/albums"_i18n : "main/music/album"_i18n));
            }
            this->recycler->setDataSource(new VideoDataSource(r.Items));
        },
        [ASYNC_TOKEN](const std::string& ex) {
            ASYNC_RELEASE
            this->recycler->setError(ex);
        });
}
