import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:collection/collection.dart';
import 'package:fl_clash/common/common.dart';
import 'package:fl_clash/common/tray.dart';
import 'package:fl_clash/common/window.dart';
import 'package:fl_clash/enum/enum.dart';
import 'package:fl_clash/providers/action.dart';
import 'package:fl_clash/providers/app.dart';
import 'package:fl_clash/providers/state.dart';
import 'package:flutter/foundation.dart' show visibleForTesting;
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:material_ui/material_ui.dart';
import 'package:path/path.dart' as path;
import 'package:tray/tray.dart';

@visibleForTesting
String serializeWindowsTrayState({
  required int port,
  required bool systemProxy,
  required List<String> bypassDomains,
}) {
  final domains = bypassDomains
      .map((domain) => domain.replaceAll(RegExp(r'[\r\n]'), ''))
      .toList();
  return [
    '$port',
    systemProxy ? '1' : '0',
    '${domains.length}',
    ...domains,
  ].join('\n');
}

class TrayManager extends ConsumerStatefulWidget {
  final Widget child;

  const TrayManager({super.key, required this.child});

  @override
  ConsumerState<TrayManager> createState() => _TrayManagerState();
}

class _TrayManagerState extends ConsumerState<TrayManager> {
  StreamSubscription<TrayEvent>? _subscription;
  bool _isUpdating = false;
  bool _hasPendingUpdate = false;
  bool _standaloneTrayStarted = false;

  bool get _usesStandaloneWindowsTray => system.isWindows && !safeModeBuild;

  @override
  void initState() {
    super.initState();
    if (!_usesStandaloneWindowsTray) {
      _subscription = Tray.instance.events.listen(_handleTrayEvent);
    }
    ref.listenManual(trayStateProvider, (prev, next) {
      if (prev != next) {
        _requestUpdate();
      }
    });
    ref.listenManual(loadedLocaleProvider, (prev, next) {
      if (prev != null && prev != next) {
        _requestUpdate();
      }
    });
    if (_usesStandaloneWindowsTray) {
      ref.listenManual(proxyStateProvider, (prev, next) {
        if (prev != next) {
          _requestUpdate();
        }
      });
      _requestUpdate();
    }
    if (system.isMacOS) {
      ref.listenManual(trayDelaysProvider, (prev, next) {
        if (!const DeepCollectionEquality().equals(prev, next)) {
          _requestUpdate();
        }
      });
      ref.listenManual(trayTitleStateProvider, (prev, next) {
        if (prev != next) {
          _reportFailure(
            appTray?.updateTitle(
              showTrayTitle: next.showTrayTitle,
              traffic: next.traffic,
            ),
          );
        }
      });
    }
  }

  /// A delay test changes the menu per proxy, so updates in flight coalesce.
  void _requestUpdate() {
    if (_isUpdating) {
      _hasPendingUpdate = true;
      return;
    }
    _isUpdating = true;
    _reportFailure(_drainUpdates());
  }

  Future<void> _drainUpdates() async {
    try {
      do {
        _hasPendingUpdate = false;
        if (_usesStandaloneWindowsTray) {
          await _updateStandaloneWindowsTray();
        } else {
          await ref.read(systemActionProvider.notifier).updateTray();
        }
      } while (_hasPendingUpdate && mounted);
    } finally {
      _isUpdating = false;
      if (_hasPendingUpdate && mounted) {
        _requestUpdate();
      }
    }
  }

  Future<void> _updateStandaloneWindowsTray() async {
    final state = ref.read(proxyStateProvider);
    final homeDirPath = await appPath.homeDirPath;
    final stateFilePath = path.join(homeDirPath, 'FlClashTray.state');
    final contents = serializeWindowsTrayState(
      port: state.port,
      systemProxy: state.systemProxy,
      bypassDomains: state.bassDomain,
    );
    final temporaryPath = '$stateFilePath.${uniqueId}.tmp';
    await File(temporaryPath).writeAsString(
      contents,
      encoding: utf8,
      flush: true,
    );
    await File(temporaryPath).rename(stateFilePath);

    if (_standaloneTrayStarted) {
      return;
    }
    await Process.start(
      path.join(appPath.executableDirPath, 'FlClashTray.exe'),
      [Platform.resolvedExecutable, stateFilePath],
      mode: ProcessStartMode.detached,
    );
    _standaloneTrayStarted = true;
  }

  void _reportFailure(Future<void>? operation) {
    if (operation == null) {
      return;
    }
    unawaited(
      operation.onError<Object>((error, stackTrace) {
        commonPrint.log(
          'Tray operation failed: ${compactError(error)}',
          logLevel: LogLevel.error,
        );
      }),
    );
  }

  void _handleTrayEvent(TrayEvent event) {
    switch (event) {
      case TrayIconActivated():
        window?.show();
      case TrayMenuRequested():
        _reportFailure(Tray.instance.openMenu());
      case TrayMenuItemSelected():
        break;
    }
  }

  @override
  Widget build(BuildContext context) {
    return widget.child;
  }

  @override
  void dispose() {
    _subscription?.cancel();
    super.dispose();
  }
}
