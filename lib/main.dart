import 'package:material_ui/material_ui.dart';
import 'package:flutter/scheduler.dart';

void main() {
  runApp(const MaterialApp(debugShowCheckedModeBanner: false, home: ScrollingList()));
}

/// A list that scrolls at a constant speed on every frame, so each display refresh should show a new frame.
class ScrollingList extends StatefulWidget {
  const ScrollingList({super.key});

  @override
  State<ScrollingList> createState() => _ScrollingListState();
}

class _ScrollingListState extends State<ScrollingList> with SingleTickerProviderStateMixin {
  static const double _rowHeight = 64;
  static const int _rows = 1000;
  static const double _pixelsPerSecond = 600;

  final ScrollController _controller = ScrollController();
  late final Ticker _ticker;

  @override
  void initState() {
    super.initState();

    _ticker = createTicker((elapsed) {
      if (!_controller.hasClients || !_controller.position.hasContentDimensions) {
        return;
      }

      final offset = elapsed.inMicroseconds / 1e6 * _pixelsPerSecond;

      _controller.jumpTo(offset % _controller.position.maxScrollExtent);
    })..start();
  }

  @override
  void dispose() {
    _ticker.dispose();
    _controller.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return ListView.builder(
      controller: _controller,
      itemExtent: _rowHeight,
      itemCount: _rows,
      itemBuilder: (context, index) => ColoredBox(
        color: HSVColor.fromAHSV(1, (index * 7) % 360, 0.5, 0.9).toColor(),
        child: Center(child: Text('Row $index', style: const TextStyle(fontSize: 24))),
      ),
    );
  }
}
