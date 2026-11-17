#pragma once
#include <QIcon>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>

inline QPixmap chatLogo(int size) {
  QPixmap image(size, size);
  image.fill(Qt::transparent);
  QPainter p(&image);
  p.setRenderHint(QPainter::Antialiasing);
  p.scale(size / 64.0, size / 64.0);
  QLinearGradient gradient(8, 4, 54, 55);
  gradient.setColorAt(0, QColor("#8271ff"));
  gradient.setColorAt(1, QColor("#5342ed"));
  p.setPen(Qt::NoPen);
  p.setBrush(gradient);
  QPainterPath path;
  path.addRoundedRect(QRectF(4, 5, 56, 44), 11, 11);
  QPainterPath tail;
  tail.moveTo(13, 45);
  tail.lineTo(13, 59);
  tail.lineTo(31, 45);
  tail.closeSubpath();
  path = path.united(tail);
  p.drawPath(path);
  p.setBrush(Qt::white);
  for (int x : {20, 32, 44})
    p.drawEllipse(QPointF(x, 27), 3, 3);
  return image;
}
inline QIcon lineIcon(const QString &kind, QColor color = QColor("#545568")) {
  QPixmap image(26, 26);
  image.fill(Qt::transparent);
  QPainter p(&image);
  p.setRenderHint(QPainter::Antialiasing);
  p.setPen(QPen(color, 1.7, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
  p.setBrush(Qt::NoBrush);
  if (kind == "chat") {
    p.drawRoundedRect(QRectF(3, 4, 20, 16), 6, 6);
    p.drawLine(6, 19, 5, 24);
    p.drawLine(5, 24, 12, 20);
  } else if (kind == "user" || kind == "friends" || kind == "add") {
    p.drawEllipse(QPointF(12, 7), 4, 4);
    p.drawArc(QRectF(3, 13, 18, 15), 0, 180 * 16);
    p.drawLine(3, 21, 21, 21);
    if (kind == "add") {
      p.drawLine(20, 10, 20, 17);
      p.drawLine(16, 13, 24, 13);
    }
  } else if (kind == "lock") {
    p.drawRoundedRect(QRectF(5, 12, 16, 11), 2, 2);
    p.drawArc(QRectF(8, 3, 10, 17), 0, 180 * 16);
    p.drawLine(13, 16, 13, 19);
  } else if (kind == "bell") {
    QPainterPath path;
    path.moveTo(4, 20);
    path.lineTo(7, 16);
    path.lineTo(7, 11);
    path.cubicTo(7, 2, 19, 2, 19, 11);
    path.lineTo(19, 16);
    path.lineTo(22, 20);
    path.closeSubpath();
    p.drawPath(path);
    p.drawArc(QRectF(10, 19, 6, 6), 180 * 16, 180 * 16);
  } else if (kind == "send") {
    QPainterPath path;
    path.moveTo(3, 11);
    path.lineTo(23, 3);
    path.lineTo(16, 24);
    path.lineTo(11, 15);
    path.closeSubpath();
    p.drawPath(path);
    p.drawLine(11, 15, 23, 3);
  } else if (kind == "search") {
    p.drawEllipse(QPointF(11, 11), 7, 7);
    p.drawLine(17, 17, 23, 23);
  } else {
    p.drawEllipse(QPointF(13, 13), 7, 7);
    p.drawEllipse(QPointF(13, 13), 2.5, 2.5);
    for (int i = 0; i < 8; ++i) {
      p.save();
      p.translate(13, 13);
      p.rotate(i * 45);
      p.drawLine(0, -7, 0, -11);
      p.restore();
    }
  }
  return QIcon(image);
}
inline QPixmap avatar(const QString &text, qint64 key, int size = 44) {
  static const QColor colors[] = {QColor("#7561dd"), QColor("#5183c0"),
                                  QColor("#62a598"), QColor("#d28d75"),
                                  QColor("#b085b8")};
  QPixmap image(size, size);
  image.fill(Qt::transparent);
  QPainter p(&image);
  p.setRenderHint(QPainter::Antialiasing);
  p.setPen(Qt::NoPen);
  p.setBrush(colors[static_cast<quint64>(key) % 5]);
  p.drawEllipse(1, 1, size - 2, size - 2);
  QString initial =
      text.isEmpty()
          ? "?"
          : text.left(text[0].isHighSurrogate() && text.size() > 1 ? 2 : 1);
  QFont font = p.font();
  font.setPixelSize(size / 2);
  font.setBold(true);
  p.setFont(font);
  p.setPen(Qt::white);
  p.drawText(image.rect(), Qt::AlignCenter, initial.toUpper());
  return image;
}
