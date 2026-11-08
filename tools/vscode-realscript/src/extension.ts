import * as path from 'path';
import * as vscode from 'vscode';
import {
  LanguageClient,
  LanguageClientOptions,
  ServerOptions,
  TransportKind,
} from 'vscode-languageclient/node';

let lspClient: LanguageClient | undefined;

async function startLanguageClient(context: vscode.ExtensionContext): Promise<void> {
  if (lspClient) {
    await lspClient.stop();
    lspClient = undefined;
  }

  const config = vscode.workspace.getConfiguration('realscript');
  const serverPath = config.get<string>('lsp.serverPath', 'rslsp');
  const serverArgs = config.get<string[]>('lsp.serverArgs', []);

  const serverOptions: ServerOptions = {
    run: {
      command: serverPath,
      args: serverArgs,
      transport: TransportKind.stdio,
    },
    debug: {
      command: serverPath,
      args: serverArgs,
      transport: TransportKind.stdio,
    },
  };

  const clientOptions: LanguageClientOptions = {
    documentSelector: [
      { scheme: 'file', language: 'realscript' },
      { scheme: 'untitled', language: 'realscript' },
    ],
    synchronize: {
      fileEvents: vscode.workspace.createFileSystemWatcher('**/*.{rs,d.rs}'),
    },
    outputChannelName: 'RealScript Language Server',
  };

  lspClient = new LanguageClient(
    'realscriptLsp',
    'RealScript Language Server',
    serverOptions,
    clientOptions
  );

  context.subscriptions.push(lspClient);
  await lspClient.start();
}

class RealScriptDebugConfigurationProvider implements vscode.DebugConfigurationProvider {
  resolveDebugConfiguration(
    _folder: vscode.WorkspaceFolder | undefined,
    config: vscode.DebugConfiguration,
    _token?: vscode.CancellationToken
  ): vscode.ProviderResult<vscode.DebugConfiguration> {
    const rsConfig = vscode.workspace.getConfiguration('realscript.debug');

    if (!config.type && !config.request && !config.name) {
      const editor = vscode.window.activeTextEditor;
      if (editor && editor.document.languageId === 'realscript') {
        config.type = 'realscript';
        config.name = 'RealScript: Attach to Running Game Engine';
        config.request = 'attach';
        config.host = rsConfig.get<string>('defaultHost', '127.0.0.1');
        config.port = rsConfig.get<number>('defaultPort', 4711);
        config.stopOnEntry = false;
      }
    }

    if (config.request === 'attach') {
      if (!config.host) {
        config.host = rsConfig.get<string>('defaultHost', '127.0.0.1');
      }
      if (!config.port || typeof config.port !== 'number') {
        config.port = rsConfig.get<number>('defaultPort', 4711);
      }
      return config;
    }

    if (config.request === 'launch') {
      if (!config.program || (Array.isArray(config.program) && config.program.length === 0)) {
        const editor = vscode.window.activeTextEditor;
        if (editor && editor.document.languageId === 'realscript') {
          config.program = [editor.document.uri.fsPath];
        } else {
          vscode.window.showErrorMessage(
            'RealScript Launch requires at least one .rs source file in "program".'
          );
          return undefined;
        }
      }
      if (!config.function) {
        config.function = 'Main::main';
      }
      return config;
    }

    return config;
  }
}

class RealScriptDebugAdapterDescriptorFactory implements vscode.DebugAdapterDescriptorFactory {
  createDebugAdapterDescriptor(
    session: vscode.DebugSession,
    _executable: vscode.DebugAdapterExecutable | undefined
  ): vscode.ProviderResult<vscode.DebugAdapterDescriptor> {
    const config = session.configuration;
    const rsConfig = vscode.workspace.getConfiguration('realscript.debug');

    // In-engine live debugging via EmbeddedDapServer TCP socket
    if (config.request === 'attach') {
      const host: string = config.host || rsConfig.get<string>('defaultHost', '127.0.0.1');
      const port: number = Number(config.port || rsConfig.get<number>('defaultPort', 4711));
      return new vscode.DebugAdapterServer(port, host);
    }

    // Standalone script debugging via rsdebug CLI over stdio (or optional TCP port)
    const adapterPath = rsConfig.get<string>('adapterPath', 'rsdebug');
    const rawPrograms = Array.isArray(config.program)
      ? config.program
      : config.program
      ? [String(config.program)]
      : [];

    const workspaceRoot = session.workspaceFolder?.uri.fsPath;
    const resolvedArgs: string[] = rawPrograms.map((p: string) =>
      workspaceRoot && !path.isAbsolute(p) ? path.resolve(workspaceRoot, p) : p
    );

    if (typeof config.port === 'number' && config.port > 0) {
      const host: string = config.host || '127.0.0.1';
      return new vscode.DebugAdapterServer(config.port, host);
    }

    return new vscode.DebugAdapterExecutable(adapterPath, resolvedArgs);
  }
}

export async function activate(context: vscode.ExtensionContext): Promise<void> {
  // 1. Register Debug Configuration Provider & Descriptor Factory (Launch + In-Engine TCP Attach)
  const provider = new RealScriptDebugConfigurationProvider();
  const factory = new RealScriptDebugAdapterDescriptorFactory();
  context.subscriptions.push(
    vscode.debug.registerDebugConfigurationProvider('realscript', provider),
    vscode.debug.registerDebugAdapterDescriptorFactory('realscript', factory)
  );

  // 2. Register Quick-Attach Command for Running Game Engine
  context.subscriptions.push(
    vscode.commands.registerCommand('realscript.attachToEngine', async () => {
      const rsConfig = vscode.workspace.getConfiguration('realscript.debug');
      const defaultHost = rsConfig.get<string>('defaultHost', '127.0.0.1');
      const defaultPort = rsConfig.get<number>('defaultPort', 4711);

      const input = await vscode.window.showInputBox({
        prompt: 'Enter host:port of the running RealScript game engine (ScriptRuntime::startRemoteDebugger)',
        value: `${defaultHost}:${defaultPort}`,
        placeHolder: '127.0.0.1:4711',
      });
      if (!input) {
        return;
      }

      const [hostPart, portPart] = input.includes(':')
        ? input.split(':', 2)
        : [defaultHost, input];
      const host = hostPart.trim() || defaultHost;
      const port = Number(portPart) || defaultPort;

      await vscode.debug.startDebugging(vscode.workspace.workspaceFolders?.[0], {
        type: 'realscript',
        request: 'attach',
        name: `RealScript Engine (${host}:${port})`,
        host,
        port,
        stopOnEntry: false,
      });
    })
  );

  // 3. Register LSP Restart Command & Start Language Server
  context.subscriptions.push(
    vscode.commands.registerCommand('realscript.restartLspServer', async () => {
      try {
        await startLanguageClient(context);
        vscode.window.showInformationMessage('RealScript Language Server restarted.');
      } catch (err) {
        vscode.window.showErrorMessage(`Failed to restart rslsp: ${String(err)}`);
      }
    })
  );

  try {
    await startLanguageClient(context);
  } catch (err) {
    // Allow debugging to work even if rslsp is not yet on PATH
    console.warn('RealScript LSP failed to start on activation:', err);
  }
}

export async function deactivate(): Promise<void> {
  if (lspClient) {
    await lspClient.stop();
    lspClient = undefined;
  }
}
