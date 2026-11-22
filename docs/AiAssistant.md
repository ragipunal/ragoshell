# The AI assistant

The assistant turns a request in plain language ("orders shipped today over 100") into one read-only `find` or
`aggregate` query for the open collection. You check the query before it runs. It is off until you turn it on in
Preferences › AI assistant.

The app has no AI service of its own. Requests go from your device straight to the provider you pick, with your own
key, or to a model running on your own computer. Nothing is sent before you press Generate, and the dialog shows
exactly what goes out: your request; the database and collection names; the collection's field names, types,
indexes and document count; the server version; and the fields of related collections in the same database.
Document values (frequent values, min/max) are sent only if you allow them in Preferences.

## Providers

| Provider | Address | Key |
|---|---|---|
| Anthropic Claude | fixed | your Anthropic API key, or `ANTHROPIC_API_KEY` |
| Google Gemini | fixed | your Google AI Studio key, or `GEMINI_API_KEY` / `GOOGLE_API_KEY` |
| OpenAI-compatible | a preset (OpenAI, OpenRouter, DeepSeek, LM Studio) or your own address | optional |
| Ollama | `http://localhost:11434/v1` by default | none |

- **The provider bills you** for its usage. A model running on your own computer costs nothing per request.
- **Test connection** asks the server for its model list. It sends no query and no schema.
- **Your key belongs to its server.** For an OpenAI-compatible server, the key is saved together with that server's
  address (scheme, host and port). If you change the address to another server, the key is cleared and is never
  sent there.
- **Environment variables** such as `OPENAI_API_KEY` are used only at their preset's own address, never at an
  address you typed.
- **Addresses must use `https://`.** Plain `http://` is accepted in two cases:
  - this device (`localhost`, `127.0.0.1`, `::1`), with or without a key;
  - on desktop, iPhone and iPad, a server on your local network, given as a private IP address (for example
    `192.168.1.20`) or a `.local` name, and only without a key. A key is never sent over plain http on a network.
- **Redirects are not followed** in the desktop and mobile apps. If a server answers with a redirect, the request
  stops and nothing is sent anywhere else. In the web version the browser handles redirects itself.

## Local models

A local model runs on your computer (or a computer on your network). It needs no key and costs nothing per request,
and the data does not leave that computer. The exceptions are a cloud model (Ollama models whose name ends in
`cloud`) and a proxy that forwards requests to an online service.

Local models are slower than online ones, most of all the first time a model loads. If you get "No answer within …
seconds", raise **Answer time limit** in Preferences (30–300 seconds; Ollama starts at 180).

### Ollama

1. Install Ollama from <https://ollama.com/download> and start it. The app runs in the background; on Linux or a
   server, run `ollama serve` instead.
2. Download a model in a terminal, for example `ollama pull llama3.2`. Models that are good at code and JSON give
   better queries. Larger models need more memory.
3. In Preferences › AI assistant, choose **Ollama**. The address is already `http://localhost:11434/v1`.
4. Press **Test connection**:
   - It lists your models. Pick one, or type its name (for example `llama3.2:latest`).
   - "Ollama is running, but no model is installed": run `ollama pull <model>` first.
   - "Cannot reach the server": Ollama is not running, or it listens on another address or port.
5. Optional: if answers seem to ignore part of the collection, the schema did not fit in the model's context.
   Raise the context length with `OLLAMA_CONTEXT_LENGTH=8192` before starting Ollama, or in the Ollama app's
   settings.

**Ollama on another computer in your network (desktop, iPhone and iPad):**

1. On that computer, start Ollama with `OLLAMA_HOST=0.0.0.0` so it accepts connections from the network.
2. In the app, enter `http://<its IP address>:11434/v1`.

Traffic on the network is plain http, so use this only on a network you trust.

### LM Studio

1. In LM Studio, download a model and start the local server: the Developer tab › Start server, or run
   `lms server start`.
2. In Preferences › AI assistant, choose **OpenAI-compatible** and then the **LM Studio** preset. The address is
   `http://localhost:1234/v1`.
3. Leave the key empty. Press **Test connection** and pick the loaded model.

### Phone and tablet

- **Android** allows plain http only to the device itself. To reach Ollama on your computer, put it behind an
  `https://` address, for example a reverse proxy with a certificate.
  - In the Android emulator, run `adb reverse tcp:11434 tcp:11434`, then use `http://localhost:11434/v1`.
- **iPhone and iPad** ask for local network access the first time the app connects to a computer on your network.

### Web version (browser)

The browser blocks a request unless the server allows the page that sent it (CORS). Plain http works only to
`localhost`.

- **Ollama:** set `OLLAMA_ORIGINS` to the address of the web app (for example
  `OLLAMA_ORIGINS=https://app.ragoshell.com.tr`), then restart Ollama.
- **LM Studio:** turn on CORS in the server settings.

If CORS is not allowed, Test connection says the server cannot be reached, with this hint.
