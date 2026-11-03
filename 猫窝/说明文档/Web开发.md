# Web 开发

Web源码保留 HTML/CSS/JS，无 CDN、外部字体或图片依赖。主控使用生成的 web_assets.h 提供压缩网页，副板不提供网页。

| 文件 | 用途 |
| --- | --- |
| index.html / styles.css | 手机页面与样式 |
| app.js / model.js | 下料、灯带、温湿度和界面逻辑 |
| camera-view.js / frame-stream.js | 双摄像头切换及帧流解码 |
| wifi-settings.js | 双板配网、丢失响应后的状态核对 |
| device-network.js | 配对设备发现、地址刷新和主界面重连 |
| wifi-scan.js | 搜索附近 Wi-Fi、合并两板列表、选择网络 |
| embed-web.mjs | 生成主控 web_assets.h |
| server.mjs | 本机静态网页预览，不连接 USB 串口 |
| package.json / package-lock.json | Node 命令与项目元数据，当前无 npm 运行依赖 |
| tests/ | 离线回归测试与显式启用的只读摄像头硬件检查 |

修改网页后从项目根目录运行，再按 [编译与烧录](编译与烧录.md) 更新主控：

```powershell
node '.\Web源码\embed-web.mjs'
```

本机开发工具：

```powershell
Set-Location '.\Web源码'
npm test
npm start
```

打开 http://127.0.0.1:5173/。本机服务只提供静态页面；摄像头、下料、灯带、AHT20 和 Wi-Fi 配置需要通过主控网页验证。已删除早期 USB 桥接程序与 serialport 依赖，本地预览不打开串口。基础测试不需要 npm ci；浏览器测试另需 Playwright 和 Edge，依赖安装在项目外。

搜索功能离线交互测试：tests/wifi-scan.browser.cjs，使用 Playwright 和本机 Edge；设置 PLAYWRIGHT_MODULE 为已安装 playwright 包路径后，用 node 执行该脚本。测试拦截所有网络请求，不连接真实设备，检查网络合并、选择、异常响应、取消等待和手机宽度布局。

新版摄像头不读取手工保存的 IP。测试 device-network.test.mjs 与 wifi-pair.browser.cjs 覆盖自动发现和配网异常；wifi-scan.browser.cjs 已更新为仅两台一起搜索。硬件测试不得假定默认 .6 地址，使用 CAMERA_DEVICE_URL。

二号摄像头画面默认旋转 180 度：camera-view.js 只对二号预览图片应用变换，保存照片时用 canvas 旋转像素。一号不旋转；不在每个视频帧上重新编码 JPEG。
