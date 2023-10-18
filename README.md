# set-lcd
对于lcd屏进行设置。从基本的安装，到设置旋转，以及是否作为桌面等功能。目前适配核桃派与树莓派64位系统。

有问题可以加qq群询问：677173708

# 安装
1. clone
```
git clone https://github.com/sc-bin/set-lcd.git
```

2. 执行安装脚本
```
cd set-lcd/
sudo ./install
```
注意安装完不要删除本文件夹

3. 使自动补全生效
运行以下命令，让自动补全生效。就是输命令的时候可以按两下tab键，会把可选项给显示出来
```
source /etc/bash_completion.d/set-lcd
```


# 使用
```
set-lcd [lcd-name] [function] [value]
```
支持自动补全，按两下tab键就知道当前位置有哪些可选项
- `[lcd-name]` 选择你的lcd屏
- `[function]` 要运行的功能
- `[value]` 有些功能需要再附带一些输入参数

## 启用lcd屏，并配置为桌面
在核桃派上：开机时如果不插hdmi，则把桌面显示到lcd屏，如果插上hdmi，则把桌面显示到hdmi屏。重启后生效

在树莓派上：无论插不插hdmi，都会把桌面显示到lcd屏。重启后生效

```
sudo set-lcd lcd35-st7796 install
```


## 旋转lcd方向
从驱动层面对lcd的方向进行旋转。重启后生效
```
sudo set-lcd lcd35-st7796 set_rotate 180
```

## 移除lcd
禁用相关驱动，恢复桌面的各项设置。重启后生效
```
sudo set-lcd lcd35-st7796 remove
```
