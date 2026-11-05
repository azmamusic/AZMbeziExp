# AZMbeziExp

カーブ編集式のエキスパンダーです。バックグラウンドノイズなどの小さな音を抑えることができます。

## 特長

- 音量の上昇（Attack）と下降（Release）にそれぞれ2次ベジェによるカーブを設定できます。
- Peak と RMS の混合比（RMS Ratio）と、ゲインの平滑化時間を Attack・Release で独立して調整できます。
- スペクトル処理やデュアルモノ処理を含まない純粋なゲイン調整のため、特性・位相に対して影響を与えません。
- ルックアヘッドによる遅延はありません。

## 注意事項

- REAPER のトラックルーチング上はステレオ入力を前提としています。他 DAW における ysfx 等を使用したモノラルトラック上の挙動は未確認です。
- 音声がプラグインに届いていれば停止中も動作しますが、ホスト側が停止中の処理を中断する場合はその限りではありません。
- ホスト側の FX バイパスは BYPASS とは別です。その間の処理の継続はホストに依存します。

![AZMbeziExp の画面。Attack と Release のカーブと検知レベルの縦棒（カーブと同色で光る）と、目標ゲインを決めているカーブの発光、SYNC と BYPASS、閉じた状態の RMS CONTROL を表示](docs/gui.png)

## 導入

JSFX に対応したホストのエフェクト用フォルダに [`jsfx/AZMbeziExp.jsfx`](jsfx/AZMbeziExp.jsfx) をコピーしてください。フォルダの場所や追加方法は使用するホストの案内を参照してください。

## 使い方

1. 歌声を再生し、グラフ上の縦棒（検知レベル）を見ながら Attack の Open と Range、Release の Close と Range を調整します。
2. 曲線上の点（またはグラフ下の Curve 欄）で開き方を調整します。Attack は入り、Release は語尾を聴きながら設定します。
3. グラフ下の段の右端（メーターの下）にある REDUCTION 欄で最大減衰量（リダクション下限）を決め、`RMS CONTROL` を開いて検知と平滑化時間を調整します。

## 操作画面

### グラフ

横軸が検知レベル（dB）、縦軸がゲイン（dB）です。

2本のラインはリダクションカーブを表しており、山吹色が Attack、白が Release です。カーブは中央の制御点をドラッグして調整することができます。

下の段に Attack と Release の **Curve／基準／Range** と **Reduction** の数値欄が用意されています。ドラッグで変更でき、クリックすると数値を入力できます。Shift + ドラッグで微調整、Ctrl + クリックでデフォルト値にリセットします。Attack / Release は基準値からの相対量（Range）で調整する形式になっており、基準値はそれぞれ Attack が Open、Release が Close です。

Reduction によって減衰量の最大値を変更することができます。

既定値は以下です。

| 項目 | 範囲 | 既定値 |
| --- | --- | --- |
| Curve | 0 - 100% | Attack 70% / Release -70% |
| Open |-90dB - 0dB | Attack −55 dB / Release -40dB (-55dB + 15dB ) |
| Close | -90dB - 0dB | Attack -65dB (-55 - 10dB) / Release -55dB | 
| Range | -0.1 - -60dB | Attack 10 dB / Release 15dB | 
| Reduction | 0 - -96dB | -60dB |


### RMS CONTROL

グラフ下の **RMS CONTROL** の見出しの矢印をクリックすると、RMS CONTROL セクションを開閉します。Peak との音量差を調整する **Sens**、RMS 計算窓 **Window**、Attack と Release に対してそれぞれ検知器へ流す波形の調整を行う **RMS Ratio** と **Smoothing Time** が用意されています。

既定値は以下です。

| 項目 | 範囲 | 既定値 |
| --- | --- | --- |
| Sens | −24 - +24 dB | +6 dB |
| Window | 1 - 200 ms | 60 ms |
| RMS Ratio | 0 - 100% | Attack 15% / Release 75% |
| Smoothing Time | 0.1 - 100 ms | Attack 4 ms / Release 120ms |

また **SYNC** を有効にすると Release にも Attack カーブを使い通常のエキスパンダーのように使うことができます。（検知回路は Attack Release 別のままです）

**BYPASS** ボタンを押すと処理を行いません。

## 備考

詳しい処理については [アルゴリズム](docs/algorithm.md) を参照してください。
