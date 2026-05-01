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
#include <QTimer>
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

    if (!cover.isNull()) {
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

void GameCardWidget::UpdateCoverPixmap(const QColor& border_color) {
    const int sz  = kCoverSize;
    const qreal r = kCoverRadius;
    const int   bw = kBorderWidth;

    QPixmap result(sz, sz);
    result.fill(Qt::transparent);
    QPainter p(&result);
    p.setRenderHint(QPainter::Antialiasing);

    p.setBrush(QColor(0x1e, 0x1e, 0x2e));
    p.setPen(Qt::NoPen);
    p.drawRoundedRect(0, 0, sz, sz, r, r);

    if (!raw_cover_.isNull()) {
        const qreal inner = r - bw * 0.5;
        QPainterPath clip;
        clip.addRoundedRect(bw, bw, sz - 2 * bw, sz - 2 * bw, inner, inner);
        p.setClipPath(clip);
        p.drawPixmap(bw, bw, sz - 2 * bw, sz - 2 * bw, raw_cover_);
        p.setClipping(false);
    }

    if (border_color != Qt::transparent && border_color.alpha() > 0) {
        p.setBrush(Qt::NoBrush);
        QPen pen(border_color, bw);
        pen.setJoinStyle(Qt::RoundJoin);
        p.setPen(pen);
        const qreal half = bw / 2.0;
        p.drawRoundedRect(QRectF(half, half, sz - bw, sz - bw), r, r);
    }

    cover_label_->setPixmap(result);
}

void GameCardWidget::AnimateScaleTo(qreal target, int duration_ms,
                                    QEasingCurve::Type easing)
{
    if (!active_anim_.isNull())
        active_anim_->stop();

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

    // Intercetta ogni QEvent::Resize sul viewport — incluso il resize
    // provocato dal passaggio a fullscreen — per rieseguire il layout
    // con dimensioni già definitive.
    viewport()->installEventFilter(this);
}

// ── eventFilter ────────────────────────────────────────────────────────────

bool GameGridWidget::eventFilter(QObject* obj, QEvent* event) {
    if (obj == viewport() && event->type() == QEvent::Resize)
        RelayoutCards();
    return QScrollArea::eventFilter(obj, event);
}

void GameGridWidget::AddGame(const QString& title, const QString& filepath,
                             const QPixmap& cover)
{
    auto* card = new GameCardWidget(title, filepath, cover, container_);
    connect(card, &GameCardWidget::Activated,
            this, &GameGridWidget::GameActivated);
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

void GameGridWidget::showEvent(QShowEvent* event) {
    QScrollArea::showEvent(event);
    // Defer: le dimensioni del viewport sono valide solo al prossimo
    // ciclo dell'event loop (prima apertura o show dopo hide).
    QTimer::singleShot(0, this, &GameGridWidget::RelayoutCards);
}

// ── RelayoutCards ──────────────────────────────────────────────────────────
// Comportamento stile Switch: il container è più grande del viewport grazie
// al padding virtuale (= metà viewport su ogni lato). Lo scroll iniziale
// viene posizionato al centro del padding così la prima card appare centrata.
// Quando una card è selezionata, ScrollToSelected la porta al centro.

void GameGridWidget::RelayoutCards() {
    int avail_w = viewport()->width();
    if (avail_w <= 0) avail_w = width();
    if (avail_w <= 0) return;

    const int avail_h = viewport()->height();
    if (avail_h <= 0) return;

    // Padding virtuale = metà viewport: garantisce che qualsiasi card,
    // anche la prima e l'ultima, possa essere centrata sullo schermo.
    virtual_padding_h_ = avail_w / 2;
    virtual_padding_v_ = avail_h / 2;

    const int cols = qMax(1, (avail_w - 2 * kPadding + kSpacing) / (kCardW + kSpacing));
    last_cols_ = cols;

    std::vector<GameCardWidget*> visible;
    for (auto* c : cards_)
        if (c->isVisible()) visible.push_back(c);

    const int n    = static_cast<int>(visible.size());
    const int rows = n == 0 ? 0 : (n + cols - 1) / cols;

    const int grid_w = cols * kCardW + (cols - 1) * kSpacing;
    const int grid_h = rows * kCardH + qMax(0, rows - 1) * kSpacing;

    // Il container include il padding virtuale su tutti e quattro i lati
    // più il kPadding estetico.
    const int total_w = 2 * (virtual_padding_h_ + kPadding) + grid_w;
    const int total_h = 2 * (virtual_padding_v_ + kPadding) + grid_h;

    container_->setFixedSize(total_w, total_h);
    container_->updateGeometry();

    // Le card partono dopo il padding virtuale + padding estetico,
    // centrate orizzontalmente nella griglia.
    const int origin_x = virtual_padding_h_ + kPadding;
    const int origin_y = virtual_padding_v_ + kPadding;

    for (int i = 0; i < n; ++i) {
        const int col = i % cols;
        const int row = i / cols;
        visible[i]->move(origin_x + col * (kCardW + kSpacing),
                         origin_y + row * (kCardH + kSpacing));
    }

    // FIX: i range degli scrollbar vengono aggiornati da Qt in modo
    // asincrono dopo setFixedSize. Schedulare il setValue al ciclo
    // successivo garantisce che maximum() sia già corretto.
    if (selected_index_ < 0) {
        // Nessuna selezione: porta lo scroll al centro del padding virtuale
        // così la prima card appare centrata nel viewport.
        const int target_h = virtual_padding_h_;
        const int target_v = virtual_padding_v_;
        QTimer::singleShot(0, this, [this, target_h, target_v]() {
            horizontalScrollBar()->setValue(target_h);
            verticalScrollBar()->setValue(target_v);
        });
    } else {
        QTimer::singleShot(0, this, &GameGridWidget::ScrollToSelected);
    }
}

// ── ScrollToSelected ────────────────────────────────────────────────────────
// Centra la card selezionata nel viewport con animazione smooth stile Switch.

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

    auto smooth = [](QPointer<QVariantAnimation>& anim_ref, QScrollBar* bar,
                     int from, int to)
    {
        if (!anim_ref.isNull())
            anim_ref->stop();
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

void GameGridWidget::NavigateGrid(int row_delta, int col_delta) {
    if (cards_.empty()) return;
    if (selected_index_ < 0) { SelectFirst(); return; }

    std::vector<int> visible_indices;
    visible_indices.reserve(cards_.size());
    for (int i = 0; i < static_cast<int>(cards_.size()); ++i)
        if (cards_[i]->isVisible())
            visible_indices.push_back(i);

    auto it = std::find(visible_indices.begin(), visible_indices.end(), selected_index_);
    if (it == visible_indices.end()) { SelectFirst(); return; }

    const int pos     = static_cast<int>(std::distance(visible_indices.begin(), it));
    const int cols    = qMax(1, last_cols_);
    const int cur_row = pos / cols;
    const int cur_col = pos % cols;

    const int new_row = cur_row + row_delta;
    const int new_col = cur_col + col_delta;

    const int total_visible = static_cast<int>(visible_indices.size());
    const int total_rows    = (total_visible + cols - 1) / cols;

    if (new_row < 0 || new_row >= total_rows) return;
    if (new_col < 0 || new_col >= cols)       return;

    const int new_pos = new_row * cols + new_col;
    if (new_pos < 0 || new_pos >= total_visible) return;

    UpdateSelection(visible_indices[new_pos]);
    ScrollToSelected();
}

QString GameGridWidget::SelectedGamePath() const {
    if (selected_index_ < 0 || selected_index_ >= static_cast<int>(cards_.size()))
        return {};
    return cards_[selected_index_]->FilePath();
}

#include "game_grid_widget.moc"