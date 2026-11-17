#pragma once
#include <QMainWindow>
#include <memory>
class MainWindow : public QMainWindow {
  Q_OBJECT
public:
  explicit MainWindow(QWidget *parent = nullptr);
  ~MainWindow() override;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
