// SPDX-FileCopyrightText: Copyright 2024 Azahar Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "citra_qt/game_grid_widget.h"

#include <QApplication>
#include <QContextMenuEvent>
#include <QEnterEvent>
#include <QFileInfo>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
#include <QScrollBar>
#include <QStyle>
#include <QVBoxLayout>

// ============================================================
//  GameCardWidget
// ============================================================

GameCardWidget::GameCardWidget(const QString& title, const QString& filepath,
                               const QPixmap& cover, QWidget* parent)
    : QFrame(parent), filepath_(filepath)
{
    setObjectName(QStringLiteral("GameCard"));
    setFixedSize(kWidth, kHeight);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::StrongFocus);

    // Rounded corners tramite stylesheet — la card stessa è un rettangolo arrotondato
    setAttribute(Qt::WA_TranslucentBackground, false);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 12, 12, 10);
    layout->setSpacing(8);
    layout->setAlignment(Qt::AlignHCenter);

    // ── Cover ──────────────────────────────────────────────────────────────
    cover_label_ = new QLabel(this);
    cover_label_->setFixedSize(kCoverSize, kCoverSize);
    cover_label_->setAlignment(Qt::AlignCenter);
    cover_label_->setScaledContents(false);
    // Nessuno stylesheet — rounded corner e border vengono disegnati
    // direttamente sul pixmap in UpdateCoverPixmap()

    if (!cover.isNull()) {
        // Scala e crop al centro per riempire il quadrato
        QPixmap scaled = cover.scaled(kCoverSize, kCoverSize,
                                      Qt::KeepAspectRatioByExpanding,
                                      Qt::SmoothTransformation);
        if (scaled.width() > kCoverSize || scaled.height() > kCoverSize) {
            const int x = (scaled.width()  - kCoverSize) / 2;
            const int y = (scaled.height() - kCoverSize) / 2;
            scaled = scaled.copy(x, y, kCoverSize, kCoverSize);
        }
        raw_cover_ = scaled;
    }
    // Disegna la cover con bordo trasparente (stato iniziale)
    UpdateCoverPixmap(Qt::transparent);

    // ── Titolo ─────────────────────────────────────────────────────────────
    title_label_ = new QLabel(this);
    title_label_->setObjectName(QStringLiteral("GameCardTitle"));
    title_label_->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
    title_label_->setWordWrap(false);
    QFontMetrics fm(title_label_->font());
    title_label_->setText(fm.elidedText(title, Qt::ElideRight, kWidth - 24));

    layout->addWidget(cover_label_, 0, Qt::AlignHCenter);
    layout->addWidget(title_label_, 0, Qt::AlignHCenter);
    layout->addStretch();

    // ── Ombra ──────────────────────────────────────────────────────────────
    shadow_ = new QGraphicsDropShadowEffect(this);
    shadow_->setBlurRadius(12);
    shadow_->setOffset(0, 4);
    shadow_->setColor(QColor(0, 0, 0, 80));
    setGraphicsEffect(shadow_);
}

// ── UpdateCoverPixmap ──────────────────────────────────────────────────────
// Ridisegna il pixmap della cover con rounded corners e un bordo colorato.
// border_color può essere Qt::transparent per lo stato normale.

void GameCardWidget::UpdateCoverPixmap(const QColor& border_color) {
    const int sz = kCoverSize;
    const qreal r = kCoverRadius;
    const int   bw = kBorderWidth;

    QPixmap result(sz, sz);
    result.fill(Qt::transparent);
    QPainter p(&result);
    p.setRenderHint(QPainter::Antialiasing);

    // 1. Sfondo scuro arrotondato (visibile solo se non c'è cover)
    p.setBrush(QColor(0x1e, 0x1e, 0x2e));
    p.setPen(Qt::NoPen);
    p.drawRoundedRect(0, 0, sz, sz, r, r);

    // 2. Immagine clippata agli angoli arrotondati (al netto del border)
    if (!raw_cover_.isNull()) {
        const qreal inner = r - bw * 0.5;
        QPainterPath clip;
        clip.addRoundedRect(bw, bw, sz - 2 * bw, sz - 2 * bw, inner, inner);
        p.setClipPath(clip);
        p.drawPixmap(bw, bw, sz - 2 * bw, sz - 2 * bw, raw_cover_);
        p.setClipping(false);
    }

    // 3. Bordo arrotondato sopra l'immagine
    if (border_color != Qt::transparent && border_color.alpha() > 0) {
        p.setBrush(Qt::NoBrush);
        QPen pen(border_color, bw);
        pen.setJoinStyle(Qt::RoundJoin);
        p.setPen(pen);
        // offset di bw/2 per centrare il tratto sul perimetro
        const qreal half = bw / 2.0;
        p.drawRoundedRect(QRectF(half, half, sz - bw, sz - bw), r, r);
    }

    cover_label_->setPixmap(result);
}



void GameCardWidget::AnimateScaleTo(qreal target, int duration_ms,
                                    QEasingCurve::Type easing)
{
    if (!active_anim_.isNull()) {
        active_anim_->stop();
        // QPointer si azzera automaticamente dopo DeleteWhenStopped
    }

    if (qFuzzyCompare(current_scale_, target)) return;

    auto* anim = new QVariantAnimation(this);
    anim->setDuration(duration_ms);
    anim->setEasingCurve(easing);
    anim->setStartValue(current_scale_);
    anim->setEndValue(target);

    connect(anim, &QVariantAnimation::valueChanged, this,
        [this](const QVariant& val) {
            current_scale_ = val.toReal();
            update();
        });

    active_anim_ = anim;
    anim->start(QAbstractAnimation::DeleteWhenStopped);
}

// ── SetSelected ────────────────────────────────────────────────────────────

void GameCardWidget::SetSelected(bool selected) {
    if (selected_ == selected) return;
    selected_ = selected;

    if (selected) {
        AnimateScaleTo(1.08, 200, QEasingCurve::OutBack);
        shadow_->setBlurRadius(28);
        shadow_->setOffset(0, 8);
        shadow_->setColor(QColor(0, 122, 255, 130));
        UpdateCoverPixmap(QColor(0x00, 0x7A, 0xFF));
    } else {
        AnimateScaleTo(1.0, 160, QEasingCurve::OutCubic);
        shadow_->setBlurRadius(12);
        shadow_->setOffset(0, 4);
        shadow_->setColor(QColor(0, 0, 0, 80));
        UpdateCoverPixmap(Qt::transparent);
    }
    update();
}

// ── paintEvent ─────────────────────────────────────────────────────────────

void GameCardWidget::paintEvent(QPaintEvent* event) {
    if (qFuzzyCompare(current_scale_, 1.0)) {
        QFrame::paintEvent(event);
        return;
    }

    // Scala dal centro della card senza spostare il widget nel layout
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    const QPointF center(width() / 2.0, height() / 2.0);
    QTransform t;
    t.translate(center.x(), center.y());
    t.scale(current_scale_, current_scale_);
    t.translate(-center.x(), -center.y());
    painter.setTransform(t);

    QFrame::paintEvent(event);
}

// ── Mouse & hover events ───────────────────────────────────────────────────

void GameCardWidget::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton)
        emit Activated(filepath_);
    QFrame::mousePressEvent(event);
}

void GameCardWidget::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton)
        emit Activated(filepath_);
    QFrame::mouseDoubleClickEvent(event);
}

void GameCardWidget::enterEvent(QEnterEvent* event) {
    if (!selected_) {
        AnimateScaleTo(1.04, 130, QEasingCurve::OutCubic);
        shadow_->setBlurRadius(20);
        shadow_->setOffset(0, 6);
        shadow_->setColor(QColor(0, 0, 0, 100));
        UpdateCoverPixmap(QColor(0, 122, 255, 100));
    }
    QFrame::enterEvent(event);
}

void GameCardWidget::leaveEvent(QEvent* event) {
    if (!selected_) {
        AnimateScaleTo(1.0, 150, QEasingCurve::OutCubic);
        shadow_->setBlurRadius(12);
        shadow_->setOffset(0, 4);
        shadow_->setColor(QColor(0, 0, 0, 80));
        UpdateCoverPixmap(Qt::transparent);
    }
    QFrame::leaveEvent(event);
}

void GameCardWidget::contextMenuEvent(QContextMenuEvent* event) {
    emit ContextMenuRequested(filepath_, event->globalPos());
}

// ============================================================
//  GameGridWidget
// ============================================================

GameGridWidget::GameGridWidget(QWidget* parent) : QScrollArea(parent) {
    setObjectName(QStringLiteral("GameGrid"));
    setWidgetResizable(false);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setFrameShape(QFrame::NoFrame);

    container_ = new QWidget(this);
    container_->setObjectName(QStringLiteral("GameGridContainer"));
    setWidget(container_);
}

void GameGridWidget::AddGame(const QString& title, const QString& filepath,
                             const QPixmap& cover)
{
    auto* card = new GameCardWidget(title, filepath, cover, container_);
    connect(card, &GameCardWidget::Activated, this, &GameGridWidget::GameActivated);
    connect(card, &GameCardWidget::ContextMenuRequested,
            this, &GameGridWidget::GameContextMenuRequested);
    cards_.push_back(card);
    card->show();
    RelayoutCards();
}

void GameGridWidget::Clear() {
    for (auto* card : cards_)
        card->deleteLater();
    cards_.clear();
    selected_index_ = -1;
}

void GameGridWidget::SelectFirst() {
    if (cards_.empty()) return;
    UpdateSelection(0);
    ScrollToSelected();
}

void GameGridWidget::NavigateBy(int delta) {
    if (cards_.empty()) return;

    const int dir   = delta > 0 ? 1 : -1;
    int       steps = std::abs(delta);
    int       next  = selected_index_;

    while (steps > 0) {
        int candidate = next + dir;
        if (candidate < 0 || candidate >= static_cast<int>(cards_.size()))
            break;
        next = candidate;
        if (cards_[next]->isVisible())
            --steps;
    }

    if (next != selected_index_ && cards_[next]->isVisible()) {
        UpdateSelection(next);
        ScrollToSelected();
    }
}

void GameGridWidget::ApplyFilter(const QString& text) {
    const QString lower = text.toLower();
    for (auto* card : cards_) {
        const bool visible =
            lower.isEmpty() ||
            QFileInfo(card->FilePath()).fileName().toLower().contains(lower);
        card->setVisible(visible);
    }
    RelayoutCards();
}

void GameGridWidget::resizeEvent(QResizeEvent* event) {
    QScrollArea::resizeEvent(event);
    RelayoutCards();
}

void GameGridWidget::RelayoutCards() {
    int avail_w = viewport()->width();
    if (avail_w <= 0) avail_w = width();
    if (avail_w <= 0) return;

    const int avail_h = viewport()->height();

    // Padding virtuale = metà viewport: garantisce che qualsiasi card,
    // anche la prima e l'ultima, possa essere centrata sullo schermo.
    virtual_padding_h_ = avail_w / 2;
    virtual_padding_v_ = avail_h / 2;

    const int cols = qMax(1, (avail_w - 2 * kPadding + kSpacing) / (kCardW + kSpacing));
    last_cols_ = cols;

    std::vector<GameCardWidget*> visible;
    for (auto* c : cards_)
        if (c->isVisible()) visible.push_back(c);

    const int rows      = visible.empty() ? 0
                        : (static_cast<int>(visible.size()) + cols - 1) / cols;
    const int content_w = cols * kCardW + (cols - 1) * kSpacing;
    const int content_h = rows * kCardH + qMax(0, rows - 1) * kSpacing;

    // Il container include il padding virtuale su tutti e quattro i lati
    // più il kPadding estetico. Le card vengono posizionate a partire da
    // (virtual_padding_h_ + kPadding, virtual_padding_v_ + kPadding).
    const int total_w = 2 * (virtual_padding_h_ + kPadding) + content_w;
    const int total_h = 2 * (virtual_padding_v_ + kPadding) + content_h;

    container_->setFixedSize(total_w, total_h);

    const int origin_x = virtual_padding_h_ + kPadding;
    const int origin_y = virtual_padding_v_ + kPadding;

    for (int i = 0; i < static_cast<int>(visible.size()); ++i) {
        const int col = i % cols;
        const int row = i / cols;
        visible[i]->move(origin_x + col * (kCardW + kSpacing),
                         origin_y + row * (kCardH + kSpacing));
    }

    // Dopo il relayout, riallinea lo scroll sulla card selezionata
    // (necessario anche al resize della finestra)
    if (selected_index_ >= 0)
        ScrollToSelected();
}

void GameGridWidget::ScrollToSelected() {
    if (selected_index_ < 0 ||
        selected_index_ >= static_cast<int>(cards_.size())) return;

    GameCardWidget* card = cards_[selected_index_];
    if (!card->isVisible()) return;

    const int vp_w = viewport()->width();
    const int vp_h = viewport()->height();

    // Centro della card nel sistema di coordinate del container
    const int card_cx = card->x() + kCardW / 2;
    const int card_cy = card->y() + kCardH / 2;

    // Scroll target: porta il centro della card al centro del viewport
    const int target_h = card_cx - vp_w / 2;
    const int target_v = card_cy - vp_h / 2;

    const int clamped_h = qBound(0, target_h, horizontalScrollBar()->maximum());
    const int clamped_v = qBound(0, target_v, verticalScrollBar()->maximum());

    // Durata proporzionale alla distanza, capped a 220ms — sensazione Switch
    auto smooth = [](QPointer<QVariantAnimation>& anim_ref, QScrollBar* bar,
                     int from, int to)
    {
        if (!anim_ref.isNull()) {
            anim_ref->stop();
            // stop() non distrugge l'oggetto, ma dopo DeleteWhenStopped
            // il puntatore viene azzerato automaticamente da QPointer
        }
        if (from == to) return;

        const int dist = std::abs(to - from);
        const int dur  = qMin(220, 80 + dist / 4);

        auto* anim = new QVariantAnimation();
        anim->setDuration(dur);
        anim->setEasingCurve(QEasingCurve::OutCubic);
        anim->setStartValue(from);
        anim->setEndValue(to);

        QObject::connect(anim, &QVariantAnimation::valueChanged,
            bar, [bar](const QVariant& v) { bar->setValue(v.toInt()); });
        // DeleteWhenStopped: Qt distrugge l'oggetto e QPointer si azzera
        anim->start(QAbstractAnimation::DeleteWhenStopped);

        anim_ref = anim;
    };

    smooth(scroll_anim_h_, horizontalScrollBar(),
           horizontalScrollBar()->value(), clamped_h);
    smooth(scroll_anim_v_, verticalScrollBar(),
           verticalScrollBar()->value(), clamped_v);
}

void GameGridWidget::UpdateSelection(int new_index) {
    if (selected_index_ >= 0 && selected_index_ < static_cast<int>(cards_.size()))
        cards_[selected_index_]->SetSelected(false);
    selected_index_ = new_index;
    if (selected_index_ >= 0 && selected_index_ < static_cast<int>(cards_.size()))
        cards_[selected_index_]->SetSelected(true);
}

void GameGridWidget::showEvent(QShowEvent* event) {
    QScrollArea::showEvent(event);
    RelayoutCards();
}