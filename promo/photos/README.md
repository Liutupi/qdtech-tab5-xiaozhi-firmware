# 真机照片接口
把 Tab5 正面（横屏、正对镜头、透明背景）的 PNG 放到本目录，命名为 `tab5_front.png`，
并新建 `tab5_front.json`，内容为屏幕显示区域的像素坐标，例如：

    {"screen": [120, 95, 1400, 815]}

`render.py` 会自动使用照片作为机身，把 1280×720 的界面画面合成到该区域。

另：`official_back.png`、`official_side.png` 是 M5Stack 官方产品图的裁切（背面带电池仓、侧面接口），
版权归 M5Stack 所有，未提交到仓库；需要重渲染时请自行放入本目录。
