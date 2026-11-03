# AzmBezierGate

歌声の入り方や語尾を整える、カーブ編集式のゲート／エキスパンダーです。REAPER の JSFX として試作しています。フルバンド・ステレオリンクで処理し、ルックアヘッドによる遅延はありません。

> **試作段階**：検知方式と画面を変更したため、既存プロジェクトやプリセットを開いたときの音は以前の版と同じにはなりません。REAPER 上での操作と聴感の確認が必要です。

小さなノイズを抑えつつ歌い出しや語尾を自然に残せるよう、グラフのカーブで開き方を調整することを目指しています。歌声と同時に鳴るノイズだけを分離して消すものではなく、小さな声を残すほどノイズも残りやすくなります。

## 導入

REAPER の `Options > Show REAPER resource path in explorer/finder...` で開くフォルダの `Effects` に [`jsfx/AzmBezierGate.jsfx`](jsfx/AzmBezierGate.jsfx) をコピーし、トラックの FX に **JS: AzmBezierGate** を追加します。

## 仕組み

Peak と RMS を並列検知し、それぞれ感度を調整します。Attack と Release は、それぞれ独立した **RMS Ratio** で Peak／RMS の線形振幅を混合し、別々のカーブで目標ゲインを求めます。

```text
入力 ─┬─ Peak ─ Peak Sens ─┬─ Attack RMS Ratio  ─ Attack カーブ ─┐
      └─ RMS  ─ RMS Sens  ─┴─ Release RMS Ratio ─ Release カーブ ┤
                                                                    ↓
                                                   上昇／下降で目標を選択
                                                                    ↓
                                             Attack／Release Time で追随 → 出力
```

各 Mix の検知値に短い傾き判定（固定 2 ms の一次平滑）をかけ、Attack 側の傾きが上向きなら Attack、そうでなければ Release 側の傾きが下向きなら Release を選びます。両方なら Attack 優先、どちらでもなければ直前の方向を維持します。方向の維持はゲインの保持ではなく、実ゲインは選択された目標へ追随します。

Peak は入力の瞬間的な絶対値、RMS は二乗平均の一次平滑です。検知器に独立した Attack／Release 処理はありません。RMS Window 自体による変化の遅れはあります。Deadband、カーブ間でのゲイン保持、Crossfade と独立した Gain Smoothing はありません。詳細は [`docs/algorithm.md`](docs/algorithm.md) を参照してください。

## 画面と操作

グラフの横軸は検知レベル（dB）、縦軸はゲイン（dB）です。山吹色が Attack、白が Release。各カーブの **Close／Open** ハンドルを横方向に、**Shape** ハンドルを斜め方向にドラッグして編集します。白い点は選択された検知レベルと実ゲインを示します。検知レベルの表示は描画間の最大値を使い、ゆっくり下降します。Peak／RMS メーターはそれぞれ感度調整後の値です。

**SYNC** を On にすると Release カーブの代わりに Attack カーブを使います。Release の設定は保持され、Off にすると戻ります。ただし Attack／Release の RMS Ratio は Sync 中も別々に効きます。

| グループ | つまみ | 内容 |
| --- | --- | --- |
| DETECTION | Peak Sens | Peak の感度。−24〜+24 dB、既定 −3 dB |
| DETECTION | RMS Sens | RMS の感度。−24〜+24 dB、既定 0 dB |
| DETECTION | RMS Window | RMS の平均時間。1〜200 ms、既定 20 ms |
| RANGE | Range | 最大減衰量。0〜96 dB、既定 30 dB |
| ATTACK | Time | ゲイン上昇時の追随時間。0.1〜100 ms、既定 1 ms |
| ATTACK | RMS Ratio | Attack 用の混合比。0% は Peak、100% は RMS。既定 0% |
| RELEASE | Time | ゲイン下降時の追随時間。0.1〜500 ms、既定 15 ms |
| RELEASE | RMS Ratio | Release 用の混合比。0% は Peak、100% は RMS。既定 100% |

つまみやハンドルはドラッグ、ホイール、右クリックでの数値入力ができます。Shift は微調整、ダブルクリックまたは Ctrl+クリックは既定値へのリセットです。

### 表示範囲

グラフの左右下にある dB 値が横軸と Peak／RMS メーターの表示範囲です。値をドラッグすると変更でき、クリックすると数値を入力できます。Shift で微調整、Enter で確定、Esc で入力をキャンセルします。グラフの空白部分をダブルクリックすると表示範囲を既定値（−90〜0 dB）に戻します。最小値と最大値は 10 dB 以上離れます。

**表示範囲を狭めると、両カーブの Close／Open の設定値自体が範囲内に Clamp され、音も変わります。** Sync 中に保持された Release の値も対象です。範囲を広げ直しても、Clamp 前の値は戻りません。Close と Open の間隔は 0.1 dB 以上に保たれます。

## 互換性と検証

既存のスライダー番号は維持し、新しい RMS Ratio は末尾に追加しています。旧 Peak Release、RMS Release、Crossfade のスライダー番号は互換性のために残りますが、処理には使われません。旧 Gain Smooth Open／Close の値は Attack／Release Time として使われます。

保存済みプロジェクトでは、検知器・方向判定の変更と表示範囲によるカーブの Clamp によって音が変わります。特に旧版で画面外に置いたカーブは、読み込み時に現在の表示範囲内へ変更されます。

REAPER 本体では、オートメーションと Undo、画面操作、サンプルレートや短い Time でのクリック、歌い出し・語尾の聴感を確認してください。

### オフライン検証

[`tests/test_ysfx.cpp`](tests/test_ysfx.cpp) は ysfx を使い、実際の JSFX の DSP、方向判定、表示範囲の Clamp と GUI 操作を検証します。ysfx を graphics 有効で別途ビルドした Linux 環境では、リポジトリルートから次のように実行できます。

```sh
g++ -std=c++17 -O2 -I"$YSFX_DIR/include" tests/test_ysfx.cpp \
  "$YSFX_BUILD_DIR/libysfx.a" -ldl $(pkg-config --libs fontconfig freetype2) -o /tmp/azm-test-ysfx
/tmp/azm-test-ysfx
```
