#include "SourceTree.hpp"
#include "SourceTreeDelegate.hpp"

#ifdef __ANDROID__
#include <utility/android-layout-policy.hpp>
#endif

#include "moc_SourceTreeDelegate.cpp"

SourceTreeDelegate::SourceTreeDelegate(QObject *parent) : QStyledItemDelegate(parent) {}

QSize SourceTreeDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const
{
	SourceTree *tree = qobject_cast<SourceTree *>(parent());
	QWidget *item = tree->indexWidget(index);

	if (!item) {
		return QStyledItemDelegate::sizeHint(option, index);
	}

	QSize hint = item->sizeHint();
#ifdef __ANDROID__
	/* Compact only in a small app window; retain tablet touch targets. */
	hint.setHeight(qMax(hint.height(),
			    OBSAndroidLayout::touchRowHeight(tree->property("androidCompactLayout").toBool())));
#endif
	return hint;
}
