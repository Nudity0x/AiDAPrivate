#include "aida_kbd_chip.hpp"

#include "../theme/aida_fonts.hpp"

namespace aida::qt::widgets {

AidaKbdChip::AidaKbdChip(QWidget* parent)
    : QLabel(parent)
{
    setObjectName(QStringLiteral("aida.kbd_chip"));
    setAlignment(Qt::AlignCenter);
    setFocusPolicy(Qt::NoFocus);
    setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
    setFont(aida::qt::theme::fonts::caption());
}

AidaKbdChip::AidaKbdChip(const QString& text, QWidget* parent)
    : AidaKbdChip(parent)
{
    setText(text);
}

}
