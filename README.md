# NETScratch for Windows
一个普通的多线程局域网IP扫描工具
## 软件由来
作者闲着没事干用AI做出来的东西<br>
（作者PS：其实网上有很多比我这个做的更好的工具了，我做这个纯属白搭）
## 支持系统
Windows 7 SP1 ~ Windows 11<br>
（Windows 7 SP1 需先安装系统补丁 KB4474419，否则 Npcap 驱动无法通过 SHA-2 签名校验）
## 软件功能
- IP占用扫描（主机发现由 [nmap](https://nmap.org/) 子进程完成）
- DHCP服务器扫描<br>
（后面再加）
## 开源协议
本项目使用 **GNU General Public License v3.0** 开源，完整协议见 [LICENSE](LICENSE)。<br>
主机发现调用随程序分发的官方 nmap，nmap 使用 NPSL 许可（与 GPLv3 不兼容），
其许可文件位于 release 目录下的 `nmap/LICENSE` 与 `nmap/3rd-party-licenses.txt`，版权归 nmap 项目所有。<br>
安装包随附官方 Npcap 安装程序（未做任何修改），安装向导最后会引导安装 Npcap 驱动。<br>
未安装 Npcap 时 nmap 自动退化为 connect() 模式，功能可用但速度较慢、拿不到 MAC 地址。<br>
Npcap 版权归 Nmap Software LLC 所有，许可条款见 https://npcap.com/#license。
## 软件截图（2026-08-04拍摄）
<img width="1920" height="1032" alt="图片" src="https://github.com/user-attachments/assets/e8a16141-37f3-4572-bc7f-21ecac421760" />
<img width="1920" height="1032" alt="图片" src="https://github.com/user-attachments/assets/308ca8cf-1a1e-492c-a66d-282b59351789" />
<img width="1920" height="1032" alt="图片" src="https://github.com/user-attachments/assets/4b3722c2-96d7-4c48-a514-b1500155ec6a" />

## 结尾

有建议或bug：[提issues](https://github.com/youye-luna/NETScratch/issues)<br>
不喜勿喷<br>
本人是真的不太会编程
