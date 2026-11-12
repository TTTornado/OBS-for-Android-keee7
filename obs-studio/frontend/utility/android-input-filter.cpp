#include "android-input-filter.hpp"

#ifdef __ANDROID__

#include <utility/android-window-fit.hpp>
#include <widgets/OBSAndroidDisplay.hpp>

#include <QAbstractItemView>
#include <QAbstractScrollArea>
#include <QApplication>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QElapsedTimer>
#include <QGestureEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPointer>
#include <QScroller>
#include <QTimer>
#include <QWidget>

/* Android 触摸/按键补齐。两件事性质相同 —— 平台把事件送进来了，但 OBS 那套桌面语义没人接 ——
 * 所以合在一个 app 级事件过滤器里：
 * ① 3-4 长按 = 右键。Android 上 Qt 把触摸转成鼠标事件（实测长按 1.2 s 只把列表项选中了，
 * 不出上下文菜单 —— TapAndHold 手势在 QWidget 这条路上没人接），而 OBS 的场景/源/混音器菜单
 * 全挂在 customContextMenuRequested 上，没有右键就永远点不出来。
 * 不拦原始事件：按下照常发给控件（选中、滚动行为都不变），只是同时起一个计时器；到点还没松手
 * 也没滑出阈值，就照 Qt 处理 Key_Menu 的做法，给"最近的声明了上下文菜单策略的祖先控件"送一条
 * QContextMenuEvent(Mouse)。触发后的那次松手**只在真的弹起了菜单时**吞掉（免得给列表补一次"点击"）；
 * 什么都没弹就照常交回控件 —— 否则长按等于把按钮的 clicked 吃掉，见 fire() 的返回值注释。
 * ② 返回键 = Escape，见下面 KeyPress 那一格（build 74）。
 * 不用 Q_OBJECT：只要 eventFilter 虚函数 + 成员 QTimer，省掉一次 moc。 */
/* 把顶层窗口压进屏幕可用区（全局兜底，B 方案）。
 *
 * 背景：OBS 的对话框尺寸是按桌面设计的 —— 添加源 1040x620、设置 981x730、
 * 高级音频 1100x340、滤镜 861x726 …，而这块手机横屏的逻辑尺寸只有 873x393，
 * 于是底部按钮和右侧内容直接落在屏外、点不到。之前零散地在几个对话框的 showEvent
 * 里"收高度"处理过（OBSBasicSettings 是上游自带，OBSBasicFilters 是本移植早期加的），
 * 但都只收高度、且没覆盖最常用的"添加源"。
 *
 * 这里改成全局兜底：任何顶层窗口显示后，只要比可用区大就压到可用区并居中。
 * 覆盖全部对话框，以后新增的也自动受益。
 *
 * 两个注意点：
 * 1) 用 singleShot(0) 延迟一拍，不在 Show 里同步 resize —— Show 那一刻布局还没跑完，
 *    此时 size() 还是 .ui 里的设计尺寸，改完会被随后的布局覆盖。
 * 2) resize() 受控件最小尺寸约束（minimumSize / 布局的 minimumSizeHint）。
 *    压不下去时"实际"会大于"目标"——日志里两个数都打出来，一眼能看出是不是被挡住了。
 *    那种情况需要单独处理（内部套 QScrollArea，或放宽布局约束）。 */
static void clampTopLevelToScreen(QWidget *w)
{
	const AndroidFitResult r = androidFitTopLevelToScreen(w);
	if (!r.resized)
		return; /* 尺寸已经合适（或本来就不该压），不产生任何可见动作 */

	/* resize 被挡住，说明这个对话框还带着"桌面尺寸假设"（.ui 里写死的 minimumSize
	 * 或某个子控件的 minimumHeight），需要在它的构造函数里放宽 —— 见
	 * OBSBasicSourceSelect / OBSBasicSettings 的 #ifdef __ANDROID__ 块。
	 *
	 * 踩过的三层留档，免得以后再摸一遍：
	 *   ① .ui 里写死 minimumHeight（添加源的 sourceTypeList 是 460）；
	 *   ② 布局首次 show 时把窗口自己的 minimumSize 设成布局最小尺寸
	 *      （QLayout 默认 SetDefaultConstraint），QWidget::resize() 被它夹住；
	 *   ③ QLayout 算最小尺寸走 qSmartMinSize()（qlayoutengine.cpp:321-333）：
	 *      sizePolicy 没有 ShrinkFlag 时取 max(sizeHint, minimumSizeHint)。
	 * ②③ 都能用"在源码里放宽最小尺寸"绕开，不需要去碰这些机制。 */
	if (r.actual != r.target) {
		/* 诊断：找出最占高度的那个子控件，够定位了（不再排前 5 名）。 */
		QString worst;
		int worstH = 0;
		for (QWidget *c : w->findChildren<QWidget *>()) {
			const int h = c->minimumSizeHint().height();
			if (h > worstH) {
				worstH = h;
				worst = QStringLiteral("%1/%2 minH=%3 sizeHint=%4")
						.arg(QString::fromLatin1(c->metaObject()->className()),
						     c->objectName())
						.arg(c->minimumHeight())
						.arg(c->sizeHint().height());
			}
		}

		/* 这里**故意不再硬压**。以前用 setMinimumSize(0,0)+SetNoConstraint 把窗口
		 * 强行缩到目标尺寸，会造出"窗口比内容小"的不一致状态：布局仍按原来的最小
		 * 尺寸摆放子控件，控件被裁在窗口外 —— 实测把添加源列表的滚动和命中判定都
		 * 搞坏了（列表 viewport 比可视区大，滚动条范围算错）。
		 *
		 * 正确做法是在对话框自己的构造函数里放宽最小尺寸，参考
		 * OBSBasicSourceSelect / OBSBasicSettings 里的 #ifdef __ANDROID__ 块。
		 * 下面这行就是待办清单：出现了就说明该对话框还没做源码级修复。 */
		obsAndroidDisplayLog(
			"CLAMP-BLOCKED %s：目标 %dx%d 实际 %dx%d；最占高度=%s —— 该对话框需要在源码里放宽最小尺寸",
			w->metaObject()->className(), r.target.width(), r.target.height(), r.actual.width(),
			r.actual.height(), worst.toUtf8().constData());
	}

	obsAndroidDisplayLog("CLAMP %s %dx%d -> 目标 %dx%d 实际 %dx%d（可用 %dx%d）", w->metaObject()->className(),
			     r.before.width(), r.before.height(), r.target.width(), r.target.height(),
			     r.actual.width(), r.actual.height(), r.avail.width(), r.avail.height());
}

/* 给对话框里的滚动区开单指滑动（QScroller）。
 *
 * 背景：Qt 在 Android 上把单指触摸合成成鼠标事件，而 QAbstractScrollArea 只在
 * wheelEvent 里滚动 —— 所以现在只有"双指拖"（Qt 把它合成成 QWheelEvent）能滚；
 * 单指拖在桌面语义里是框选/拖拽，不滚。这是手机上最不习惯的一点。
 *
 * 用 TouchGesture（而不是 LeftMouseButtonGesture）：
 *   TouchGesture 会给控件设 WA_AcceptTouchEvents（见 qscroller.cpp 的 grabGesture
 *   :372-373），直接吃原始触摸事件 —— 滚动时**不会先按下选中列表项**，
 *   这才是 Android 上正常的观感。
 *   LeftMouseButtonGesture 走的是合成鼠标事件，拖动会先触发一次 press，
 *   列表项被顺带选中，观感差一截。
 *
 * 代价与风险：控件接受触摸后，Qt 不再为它合成鼠标事件，**轻点是否还能选中/触发
 * 列表项必须真机验证**。若点击失灵，把这里换回 LeftMouseButtonGesture 即可
 * （它不动 WA_AcceptTouchEvents，鼠标合成照旧，代价是滚动时会误选中一次）。
 *
 * 只作用于对话框：本函数只从"非主窗"的 Show 分支里调，所以主窗口里的
 * 场景/源列表不受影响 —— 那两个有拖拽排序，单指拖必须留给拖拽。 */
static void enableFlickScroll(QWidget *w)
{
	if (!w || !w->isWindow())
		return;

	/* 排除表必须和 clampTopLevelToScreen 保持一致。尤其 Popup：QComboBox 的弹层
	 * （QComboBoxPrivateContainer）是 Qt::Popup，给它挂 TouchGesture 会改变它的
	 * 触摸/鼠标合成行为 —— 而那正是 P-8 补丁（组合框弹层被松手打死）刚修好的链路。 */
	const Qt::WindowFlags f = w->windowFlags();
	if (f.testFlag(Qt::Popup) || f.testFlag(Qt::ToolTip))
		return;

	const QList<QAbstractScrollArea *> areas = w->findChildren<QAbstractScrollArea *>();
	int grabbed = 0;
	int itemViews = 0;
	for (QAbstractScrollArea *area : areas) {
		QWidget *vp = area->viewport();
		if (!vp || QScroller::hasScroller(vp))
			continue; // 没有 viewport 或已经抓过，别重复挂

		/* 手势模式必须按控件类型分开，实测两种都对不上是行不通的：
		 *
		 * QAbstractItemView（QListWidget/QTreeView…）用 TouchGesture **不生效**：
		 * 它自己处理触摸，识别器抢不到，事件退回成"按住左键拖选"。在添加源的源列表上
		 * 表现就是"列表不滚、手指滑过哪项就把哪项点开"——因为
		 * OBSBasicSourceSelect 把 currentItemChanged 接到了 sourceTypeSelected，
		 * 拖选改一次当前项，右边面板就换一次。
		 *
		 * 对这类控件改用 LeftMouseButtonGesture：QScroller 会抓走鼠标并消费移动事件，
		 * 拖动变成滚动，视图收不到那些移动。代价是按下那一项仍会被选中一次
		 * （按下先到控件），但那比"依次点开所有项"好得多。
		 *
		 * QScrollArea（设置页那种）继续用 TouchGesture —— 那边实测工作正常，
		 * 而且不会出现"按下先选中"。 */
		const bool isItemView = qobject_cast<QAbstractItemView *>(area) != nullptr;
		QScroller::grabGesture(vp, isItemView ? QScroller::LeftMouseButtonGesture
						      : QScroller::TouchGesture);
		++grabbed;
		if (isItemView)
			++itemViews;
	}

	obsAndroidDisplayLog("FLICK %s：滚动区 %d 个，本次开 %d 个（其中 item view %d 个用鼠标手势）",
			     w->metaObject()->className(), int(areas.size()), grabbed, itemViews);
}

class AndroidInputFilter : public QObject {
public:
	explicit AndroidInputFilter(QObject *parent, QWidget *(*mainWindow_)()) : QObject(parent), mainWindow(mainWindow_)
	{
		timer.setSingleShot(true);
		/* 到点只是"长按到位"，不当场弹菜单 —— OBS 的上下文菜单结尾是阻塞的 popup.exec()，
		 * 手指还按着的时候弹出去，会被随后那发 touch 当成"点在菜单外"立刻 dismiss
		 * （2026-09-05 20:36 实测：sendEvent 同毫秒返回，一句 setVisible(false) 都没有）。
		 * 真正的弹出发放延到松手那一刻。 */
		connect(&timer, &QTimer::timeout, this, [this] { armed = true; });
	}

protected:
	bool eventFilter(QObject *watched, QEvent *ev) override
	{
		if (!watched->isWidgetType())
			return false;

		switch (ev->type()) {
		/* Android 的返回键 = Escape。这一条是"先量再修"量出来的，而且**推翻了动手前的猜测**：
		 * BACK 不是没送进 Qt —— build 73 实测（`.qoder/back-measure.sh`、留档 `.qoder/backprobe/`）
		 * 它以 QEvent::KeyPress、key=Qt::Key_Back(0x1000061，qnamespace.h:855)、nativeVirtualKey=0
		 * 正常进来，沿焦点链一路传播到底没人接（主界面那一拍 6 个控件各打一行、投影仪那一拍只到
		 * OBSProjector），而**同一个窗**上紧接着发 ESCAPE(111)=Qt::Key_Escape(0x1000000) 就被
		 * OBSProjector.cpp:63 那条 QAction 接走并关掉了窗 ⇒ 缺的只有"翻译"这一环：前端全树对
		 * Qt::Key_Back 零处理（grep frontend/ 命中 0）。
		 * 为什么换成 Escape 而不是直接 close()/reject()：投影仪的关闭链是
		 * EscapeTriggered → OBSBasic::DeleteProjector（键盘 :65 与触摸菜单 :310 两条路都走它），
		 * 自己 close() 会绕过它；而根界面上浮动 dock 之类也是顶层窗，close() 会把它直接收掉。
		 * 送 Escape 就等于把"桌面上按 Escape 会发生什么"原样搬过来：QDialog 靠 app_event_filter:266
		 * 放行 + 它自己的 keyPressEvent reject，投影仪/Multiview 靠那条快捷键。 */
		case QEvent::KeyPress: {
			QKeyEvent *ke = static_cast<QKeyEvent *>(ev);
			if (ke->key() != Qt::Key_Back)
				break;
			/* 菜单/popup 立着时 Qt 自己就把它撤了（09-07 00:26 实测 BACK 能关 popup 菜单），不抢。 */
			if (QApplication::activePopupWidget())
				break;
			QWidget *top = static_cast<QWidget *>(watched)->window();
			/* 焦点还在根界面上 ⇒ BACK 留给 Android。今天它在主界面上什么都不做（01:39 实测
			 * pid 不变、chrome 仍 39,42,51），"退回后台"是另一笔账，不混进这一条里改。 */
			if (top == (mainWindow ? mainWindow() : nullptr))
				break;
			backEaten = true;
			QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
			QApplication::sendEvent(top, &esc);
			obsAndroidDisplayLog("BACK→Escape：%s（模态=%d 送出后 accepted=%d）", top->metaObject()->className(),
					     (int)top->isModal(), (int)esc.isAccepted());
			return true;
		}
		case QEvent::KeyRelease:
			/* 按下那拍已经当成 Escape 用掉了；留着这次松手只会再走一遍"BACK 没人处理"。 */
			if (backEaten && static_cast<QKeyEvent *>(ev)->key() == Qt::Key_Back) {
				backEaten = false;
				return true;
			}
			break;
		/* ===== P-8 探针（先量再修）：组合框弹层在合成触摸下打不开 ==================================
		 * 要分开的两种长相，在截图上是同一张脸（都是"两秒后屏上没有弹层"）：
		 *   A 从未 Show ⇒ `QComboBox::mousePressEvent` 里那条 `d->popup()` 根本没跑到（事件没进来 / 被手势
		 *     判走 / 落点不是那个控件）；
		 *   B Show 了又立刻 Hide ⇒ 弹层立起来又被打死，看 Hide 隔了多少毫秒、松手落在谁身上。
		 * 四行各管一环：P8-PRESS（按下落到谁、是不是 QComboBox）、P8-G（有没有被判成 TapAndHold 手势）、
		 * P8-W（顶层窗 Show/Hide/Close/Expose 与几何）、P8-REL（松手时谁是 activePopupWidget）。
		 * 前缀一律 ASCII —— 设备端 grep 中文不可靠（仪器账第 7 次）。 */
		case QEvent::Show:
		case QEvent::Hide:
		case QEvent::Close:
		case QEvent::Expose: {
			QWidget *pw = static_cast<QWidget *>(watched);
			/* 主窗不进这条：它的 Expose 每轮都来，会把上面几行淹掉。 */
			if (!pw->isWindow() || pw == (mainWindow ? mainWindow() : nullptr))
				break;
			const Qt::WindowFlags flags = pw->windowFlags();
			const QRect geo = pw->geometry();
			const char *kind = ev->type() == QEvent::Show
						   ? "SHOW"
						   : ev->type() == QEvent::Hide
							     ? "HIDE"
							     : ev->type() == QEvent::Close ? "CLOSE" : "EXPOSE";
			obsAndroidDisplayLog(
				"P8-W %s cls=%s obj=%s geo=%dx%d+%d+%d popup=%d tool=%d dialog=%d vis=%d act=%d", kind,
				pw->metaObject()->className(), pw->objectName().toUtf8().constData(), geo.x(), geo.y(),
				geo.width(), geo.height(), int(flags.testFlag(Qt::Popup)), int(flags.testFlag(Qt::Tool)),
				int(flags.testFlag(Qt::Dialog)), int(pw->isVisible()),
				int(qApp->activePopupWidget() == pw));

			/* 顶层窗口显示后做两件事，都延后一拍等布局跑完：
			 * ① 压进屏幕可用区（B 方案，见 clampTopLevelToScreen）
			 * ② 给里面的滚动区开单指滑动（见 enableFlickScroll） */
			if (ev->type() == QEvent::Show)
				QTimer::singleShot(0, pw, [pw] {
					clampTopLevelToScreen(pw);
					enableFlickScroll(pw);
				});
			break;
		}
		case QEvent::Gesture: {
			QGestureEvent *ge = static_cast<QGestureEvent *>(ev);
			if (QGesture *g = ge->gesture(Qt::TapAndHoldGesture))
				obsAndroidDisplayLog("P8-G tapandhold state=%d recv=%s accepted=%d", (int)g->state(),
						       watched->metaObject()->className(), int(ev->isAccepted()));
			break;
		}
		case QEvent::MouseButtonPress: {
			QWidget *pw = static_cast<QWidget *>(watched);
			const QRect geo = pw->geometry();
			obsAndroidDisplayLog("P8-PRESS cls=%s obj=%s geo=%dx%d+%d+%d combo=%d win=%s",
					     pw->metaObject()->className(), pw->objectName().toUtf8().constData(), geo.x(),
					     geo.y(), geo.width(), geo.height(),
					     int(qobject_cast<QComboBox *>(pw) != nullptr),
					     pw->window() ? pw->window()->metaObject()->className() : "?");
			QMouseEvent *me = static_cast<QMouseEvent *>(ev);
			if (me->button() != Qt::LeftButton)
				break;
			/* P-8 修法 v3（组合框）。b83 实测（.qoder/p8-b83/p8.txt）：弹层从来不是"打不开"，
			 * 而是"开在松手之前的 1 ms 里、又被这发松手关掉"——
			 *   41.811 PRESS→QComboBox「simpleOutRecFormat」 / 41.815 SHOW→QComboBoxPrivateContainer
			 *   / 41.816 REL（落到主窗那个 QWidget）+ HIDE，**同一毫秒**。
			 * 同一处按满 400 ms（G2）弹层就活得好好的，截图 G2-combo.png 里两档混合容器都立着
			 * ⇒ 弹层本身能建、能画、能收后续点击，坏的只有"这发太快的松手"：
			 *   `QComboBoxPrivateContainer::eventFilter` 对落在弹层外的 MouseButtonRelease 一律 hidePopup，
			 *   而 Android 上这发松手送到的是主窗的 widget，不是刚建出来的弹层。
			 * ⇒ 最小干预：**不替 Qt 弹**（b83 证明 Qt 自己会弹），只记下"这发按下落在组合框上"，
			 *   到松手那一刻再看弹层立没立着（见下面 release 分支）。
			 * 为什么不在这里判定：v1 就是在这儿读 `activePopupWidget()` 想确认"弹起来了"，实测恒为 null
			 *   （Qt 登记活动弹层要到下一轮事件循环，SHOW 行里的 act=0 是同一件事）⇒ 判定必须放到松手时。
			 * 弹层已经立着时不记：那一下该由 Qt 的"点外面就关"收尾，不抢。
			 * 长按那套（armed/timer）这里一并跳过：弹层刚立起来，再补一发合成右键只会把它按死。 */
			if (QComboBox *cb = qobject_cast<QComboBox *>(pw)) {
				if (!qApp->activePopupWidget())
					comboPress = cb;
				break;
			}
			target = static_cast<QWidget *>(watched);
			pressGlobal = me->globalPosition().toPoint();
			armed = false;
			timer.start(HOLD_MS);
			break;
		}
		case QEvent::MouseMove: {
			QMouseEvent *me = static_cast<QMouseEvent *>(ev);
			if ((me->globalPosition().toPoint() - pressGlobal).manhattanLength() > MOVE_SLOP) {
				timer.stop(); // 这是拖动/滚动，不是长按
				armed = false;
			}
			break;
		}
		case QEvent::MouseButtonRelease:
		case QEvent::MouseButtonDblClick:
			if (ev->type() == QEvent::MouseButtonRelease) {
				QMouseEvent *re = static_cast<QMouseEvent *>(ev);
				QWidget *rp = static_cast<QWidget *>(watched);
				QWidget *pop = qApp->activePopupWidget();
				obsAndroidDisplayLog("P8-REL cls=%s at=(%d,%d) actPopup=%s eaten=%d", rp->metaObject()->className(),
							     re->globalPosition().toPoint().x(), re->globalPosition().toPoint().y(),
							     pop ? pop->metaObject()->className() : "none", int(comboPress != nullptr));
			}
			/* P-8 修法 v3（接上面 press 分支）：这发松手如果正好是"刚落在组合框上的那次按下"的尾巴、
			 * 而此刻弹层确实立着，就把它吃掉 —— Qt 那条"松手落在弹层外 ⇒ hidePopup"收不到事件，弹层留住。
			 * 判定放在这里（而不是按下时）是因为活动弹层要等下一轮事件循环才登记得上，见上面那段。
			 * 指针一进分支就清 ⇒ 最多多吃一发，绝不会波及后面不相干的松手（拖滚动条那类）。 */
			if (comboPress) {
				QWidget *pop = qApp->activePopupWidget();
				comboPress = nullptr;
				if (pop && pop->inherits("QComboBoxPrivateContainer")) {
					obsAndroidDisplayLog("P8-EAT 吃掉这发松手：弹层=%s 立着", pop->metaObject()->className());
					return true;
				}
			}
			timer.stop();
			if (armed) {
				armed = false;
				/* 只有真的弹起了菜单才吞这发松手。吞的理由（对列表那类成立）：既不给它补一次
				 * "点击"，也不让它以为按键还开着。但**没弹出任何东西的长按**本来就是"一次按得
				 * 久一点的点击" —— 一律吞掉就把只有 `clicked` 一个入口的按钮判了死刑（转场 "+"：
				 * 03:45 实测长按 1.2 s 三发全 `菜单立过=0`，而同一处一发 tap 就弹菜单）。 */
				if (!fire())
					return false;
				return true;
			}
			break;
		default:
			break;
		}
		return false;
	}

private:
	static constexpr int HOLD_MS = 500;
	static constexpr int MOVE_SLOP = 24;

	/* 返回值 = "这发长按真的立起过菜单吗"。松手分支要用（见 eventFilter 里 MouseButtonRelease
	 * 那段）：立过才吞松手，没立就把它交回控件 —— 否则只有 `clicked` 一个入口的按钮
	 * （转场 "+" 就是）在手指按久一点时变成"点了没反应"。03:45 实测：三条手势形态各长按 1.2 s，
	 * `锚「transitionAdd」` 一次都没新增、`菜单立过=0`。三条出口各自量耗时 > 0 就算立过 ——
	 * OBS 的菜单结尾是阻塞的 popup.exec()，同毫秒返回的那条路根本没弹东西。 */
	bool fire()
	{
		QWidget *w = target.data();
		if (!w)
			return false;

		/* 按下事件落在的是 QAbstractScrollArea 的 viewport，而策略设在列表控件自己身上；
		 * 往上找到那个声明了策略的祖先，pos 也按它的坐标系给 —— OBS 的 handler 里是 itemAt(pos)。
		 * build 80 在这条向下的路之前先补一次**向下的几何落点**：04:42 实测预览区那发按下的接收者
		 * 是**顶层窗自己**（`w「OBSBasic」1067x667+0+0 顶层=1`），而 `preview` 那条
		 * `Qt::CustomContextMenu` 设在它**自己身上**（`forms/OBSBasic.ui:240`）—— 既不在 w 上、
		 * 也不在 w 的祖先里 ⇒ 只往上爬永远找不到它。`childAt` 是纯几何的下钻（不看事件派发怎么
		 * 选接收者），拿同一个 pressGlobal 在 w 的子树里再落一次点。**只在往下落空了之后才走老路**，
		 * 且老路 `policyUp(w->parentWidget())` 与 build 79 逐字同形 ⇒ 现在绿着的那些腿一步没动。 */
		auto policyUp = [](QWidget *x) {
			while (x) {
				const Qt::ContextMenuPolicy policy = x->contextMenuPolicy();
				if (policy == Qt::CustomContextMenu || policy == Qt::ActionsContextMenu)
					return x;
				x = x->parentWidget();
			}
			return static_cast<QWidget *>(nullptr);
		};
		QWidget *const hit = w->childAt(w->mapFromGlobal(pressGlobal));
		QWidget *menu = hit ? policyUp(hit) : nullptr;
		if (!menu)
			menu = policyUp(w->parentWidget());
		if (!menu)
			menu = w;

		/* 给 3-4 探针一个能 grep 的确定信号：光看"菜单有没有弹"分不清是长按没触发，
		 * 还是触发了但那个控件压根没挂菜单。
		 * 行尾那三个字段是 04:1x 之后加的：只有 className 时"接收=OBSBasic"这一格无法自证 ——
		 * 预览中心那三个落点报的就是 OBSBasic，可落点明明在显示面自己的那个原生子窗（#7）里。
		 * ⇒ 补 objectName（同一张 .ui 里独一份）、它在父系里的几何、它自己是不是顶层窗、
		 *   以及它所属顶层窗的 objectName。**一律追加在行尾**，前面 `%s@(%d,%d) → %s` 那截不动 ——
		 *   `.qoder/longpress-hit-probe.sh` 与 `menupos-probe.sh` 的正则都吃这个前缀。 */
		const QRect wGeo = w->geometry();
		const QWidget *wWin = w->window();
		obsAndroidDisplayLog("3-4 长按→右键：%s@(%d,%d) → %s %s〔w「%s」%dx%d+%d+%d 顶层=%d 属于「%s」〕",
				     w->metaObject()->className(), pressGlobal.x(), pressGlobal.y(),
				     menu->metaObject()->className(),
				     menu == w ? "（未找到菜单策略祖先，投给事件原点）" : "",
				     qUtf8Printable(w->objectName()), wGeo.width(), wGeo.height(), wGeo.x(),
				     wGeo.y(), int(w->isWindow()), qUtf8Printable(wWin ? wWin->objectName() : QString()));

		/* 单独一行、不动上面那个前缀：记下"向下的几何落点找到了谁、最后采用的是谁"。
		 * 这一格是 build 80 那次改动唯一的判据来源 —— 没有它，"菜单起来了"和"菜单为什么
		 * 起来了"在日志里是同一张脸。 */
		const QString hitDesc = hit ? QStringLiteral("「%1」%2").arg(hit->objectName(),
									     QString::fromUtf8(
										     hit->metaObject()->className()))
					    : QStringLiteral("空");
		const QByteArray hitUtf8 = hitDesc.toUtf8();
		const QByteArray tailUtf8 =
			(menu == w ? QStringLiteral("（落点也没找到策略持有者）") : QString()).toUtf8();
		obsAndroidDisplayLog("3-4 长按→下钻：childAt=%s 采用=「%s」%s", hitUtf8.constData(),
				     qUtf8Printable(menu->objectName()), tailUtf8.constData());

		/* 坐标系：QAbstractScrollArea 那一类（SourceTree/SceneTree 都是）的
		 * `customContextMenuRequested` 按 Qt 文档给的是 **viewport 系**坐标 —— OBS 的 handler 里
		 * 就是 `itemAt(pos)`，而 `menu` 是带 2px 边框的外层控件。照外层算会整体偏一个边框宽：
		 * 02:32 实测手指在逻辑(160,76)、菜单却起于(162,78)；行边界上这一偏还会选错行。 */
		QWidget *coordBase = menu;
		if (QAbstractScrollArea *scroll = qobject_cast<QAbstractScrollArea *>(menu))
			coordBase = scroll->viewport();
		const QPoint local = coordBase->mapFromGlobal(pressGlobal);
		QContextMenuEvent ce(QContextMenuEvent::Mouse, local, pressGlobal);
		/* 分岔口不是 isAccepted()：QContextMenuEvent 造出来就是 accepted 的，CustomContextMenu
		 * 那条分支也只是 emit 一下就 return，谁都没碰过这个标志位 —— 20:36 那句"已受理"是假信号。
		 * 真正的判据是**耗时**：OBS 的 CreateSourcePopupMenu 结尾是阻塞的 popup.exec()，菜单只要
		 * 立起来，sendEvent 就要待到菜单关掉才返回（手指抬起时）；同毫秒返回 = 菜单压根没弹。 */
		QElapsedTimer held;
		held.start();
		QApplication::sendEvent(menu, &ce);
		const long long spent = (long long)held.elapsed();
		obsAndroidDisplayLog("3-4 长按→右键：sendEvent 返回 用时=%lldms 弹出菜单=%s", spent,
				     qApp->activePopupWidget() ? "在场" : "已关");

		if (spent > 0)
			return true; // 阻塞过 = 菜单真的立起来过
		/* 0ms 返回就补一发直发 —— 20:45 实测过：sendEvent 到 SourceTree/SceneTree 这类
		 * CustomContextMenu 控件，Android 上**不会**走到 OBSBasic 的 handler（0ms 返回、无菜单），
		 * 而 `invokeMethod("customContextMenuRequested")` 一发就中（菜单 33 项，藏面正常）。
		 * sendEvent 还是留着先走一遍：contextMenuEvent 覆写型控件（如 VolumeControl）只认事件不认信号。 */
		{
			obsAndroidDisplayLog("3-4 长按→右键：0ms 返回 → 直发 customContextMenuRequested(%d,%d)",
					     local.x(), local.y());
			/* 判据不能是 invokeMethod 的返回值：`customContextMenuRequested` 是 **QWidget**
			 * 自己声明的信号，任何 widget 上都"存在"，invokeMethod 恒返回 true —— 18:58 实测
			 * 那条 !ok 分支永远进不去，投影仪长按什么也没弹。
			 * 换成和上面同一把尺子：**耗时**。OBS 的 handler 结尾是阻塞的 popup.exec()，菜单
			 * 立起来 invokeMethod 就要等菜单关掉才返回；0ms = 信号发了但没人接。 */
			QElapsedTimer held2;
			held2.start();
			const bool ok = QMetaObject::invokeMethod(menu, "customContextMenuRequested",
								  Q_ARG(QPoint, local));
			const long long spent2 = (long long)held2.elapsed();
			if (ok && spent2 > 0) {
				obsAndroidDisplayLog("3-4 长按→右键：直发已处理 用时=%lldms（菜单弹过又关了）", spent2);
				return true;
			}
			obsAndroidDisplayLog("3-4 长按→右键：直发无人接（ok=%d 用时=%lldms）→ 补发右键到 %s",
					     ok, spent2, w->metaObject()->className());
			/* 还有一格：既不吃 QContextMenuEvent、直发信号又没人接的控件 —— `OBSProjector`
			 * 就是（它的右键菜单写在 mousePressEvent 的 RightButton 分支里，结尾同样是
			 * 阻塞的 popup.exec()）。Android 上顶层窗没有标题栏、KEYCODE_BACK 又实测关不掉
			 * 任何顶层窗 ⇒ 长按弹那个含"关闭"的菜单是投影仪**唯一**能触摸关掉它的入口。 */
			/* 补发右键。注意别指望把光标挪到手指处：19:03 实测 Android 上
			 * `QCursor::setPos()` 是空操作（setPos(533,333) 之后 pos() 仍返回上一次 tap 的
			 * 377,279），而菜单定位读的就是 pos() —— 所以修在菜单那一头
			 * （OBSProjector::mousePressEvent 改用 event->globalPosition()）。 */
			QElapsedTimer held3;
			held3.start();
			QMouseEvent press(QEvent::MouseButtonPress, QPointF(local), QPointF(pressGlobal),
					  Qt::RightButton, Qt::RightButton, Qt::NoModifier);
			QApplication::sendEvent(w, &press); // 到这里阻塞，直到菜单被点掉
			QMouseEvent release(QEvent::MouseButtonRelease, QPointF(local), QPointF(pressGlobal),
					    Qt::RightButton, Qt::NoButton, Qt::NoModifier);
			QApplication::sendEvent(w, &release);
			const long long spent3 = (long long)held3.elapsed();
			obsAndroidDisplayLog("3-4 长按→右键：补发的右键菜单已关 用时=%lldms", spent3);
			return spent3 > 0;
		}
	}

	QWidget *(*mainWindow)() = nullptr;
	QTimer timer{this};
	QPointer<QWidget> target;
	QPoint pressGlobal;
	bool armed = false;
	bool backEaten = false;
	QPointer<QComboBox> comboPress; // P-8：这发按下落在组合框上（松手时据此决定吃不吃）
};


QObject *androidCreateInputFilter(QObject *parent, QWidget *(*mainWindow)())
{
	return new AndroidInputFilter(parent, mainWindow);
}

#endif // __ANDROID__
