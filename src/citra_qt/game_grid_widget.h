// SPDX-FileCopyrightText: Copyright 2024 Azahar Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <vector>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QLabel>
#include <QScrollArea>
#include <QPointer>
#include <QVariantAnimation>
#include <QWidget>

class GameCardWidget : public QFrame {
    Q_OBJECT
    Q_PROPERTY(bool selected READ IsSelected WRITE SetSelected)

public:
    explicit GameCardWidget(const QString& title, const QString& filepath,
                            const QPixmap& cover, QWidget* parent = nullptr);

    QString FilePath() const { return filepath_; }
    bool IsSelected() const { return selected_; }
    void SetSelected(bool selected);

signals:
    void Activated(const QString& filepath);
    void ContextMenuRequested(const QString& filepath, const QPoint& global_pos);

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    void AnimateScaleTo(qreal target, int duration_ms, QEasingCurve::Type easing);
    void UpdateCoverPixmap(const QColor& border_color);

    QString filepath_;
    bool selected_ = false;
    qreal current_scale_ = 1.0;
    QPointer<QVariantAnimation> active_anim_;

    QGraphicsDropShadowEffect* shadow_;
    QLabel* cover_label_;
    QLabel* title_label_;
    QPixmap raw_cover_;

    // Dimensioni stile Switch: cover quadrata grande, card alta
    static constexpr int kWidth       = 250;
    static constexpr int kHeight      = 300;
    static constexpr int kCoverSize   = 220;
    static constexpr int kCoverRadius = 12;
    static constexpr int kBorderWidth = 4;
};

// -------------------------------------------------------

class GameGridWidget : public QScrollArea {
    Q_OBJECT

public:
    explicit GameGridWidget(QWidget* parent = nullptr);

    void AddGame(const QString& title, const QString& filepath, const QPixmap& cover);
    void Clear();
    void SelectFirst();
    void NavigateBy(int delta);
    void NavigateGrid(int row_delta, int col_delta);
    void ApplyFilter(const QString& text);
    void RelayoutCards();

    QString SelectedGamePath() const;
    bool    HasSelection() const { return selected_index_ >= 0; }
    int     CurrentColumns() const { return last_cols_; }

    QString SelectedFilePath() const {
        if (selected_index_ < 0 ||
            selected_index_ >= static_cast<int>(cards_.size()))
            return {};
        return cards_[selected_index_]->FilePath();
    }

signals:
    void GameActivated(const QString& filepath);
    void GameContextMenuRequested(const QString& filepath, const QPoint& global_pos);

protected:
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;
    bool eventFilter(QObject* obj, QEvent* event) override;

private:
    void UpdateSelection(int new_index);
    void ScrollToSelected();
    void CenterScrollOnOrigin();

    QWidget* container_;
    std::vector<GameCardWidget*> cards_;
    int selected_index_ = -1;
    int last_cols_      = 1;

    QPointer<QVariantAnimation> scroll_anim_h_;
    QPointer<QVariantAnimation> scroll_anim_v_;

    int virtual_padding_h_ = 0;
    int virtual_padding_v_ = 0;

    // Quante volte abbiamo già eseguito il layout (usato per il
    // doppio-defer allo startup: il primo ciclo aggiorna il container,
    // il secondo aggiorna i range degli scrollbar).
    int layout_generation_ = 0;

    static constexpr int kCardW   = 230;   // deve matchare kWidth - margini card
    static constexpr int kCardH   = 280;   // deve matchare kHeight - margini card
    static constexpr int kSpacing = 24;
    static constexpr int kPadding = 28;
};