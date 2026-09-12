# Elicitation

Elicitation allows the server to request additional input from the user during tool execution. It replaces the deprecated Sampling mechanism (SEP-2577).

## Form Mode

Requests structured user input via a JSON Schema form:

```cpp
ElicitRequestParams params;
params.message = "Please provide your shipping address";
params.requested_schema = JsonValue::Parse(R"({
    "type": "object",
    "properties": {
        "street": {"type": "string"},
        "city": {"type": "string"},
        "zip": {"type": "string"}
    },
    "required": ["street", "city", "zip"]
})");

auto future = server->Elicit(params);
auto result = future.get();
if (result.content) {
    auto street = (*result.content)["street"];
}
```

## Elicitation Result

`ElicitResult` extends `Result` with:

| Field | Type | Description |
|-------|------|-------------|
| `action` | `string` | `"accept"`, `"decline"`, or `"cancel"` |
| `content` | `optional<JsonValue>` | Submitted form data (present on accept) |

The inherited `result_type` (`Complete` or `InputRequired`) indicates whether the input was fulfilled or is still pending.

## Typed Helper

`ElicitResultTyped<T>` is a user-side convenience struct for wrapping a deserialized result:

```cpp
struct AddressForm {
    std::string street;
    std::string city;
    std::string zip_code;
};

ElicitResult raw = future.get();
ElicitResultTyped<AddressForm> typed;
if (raw.content) {
    typed.action = "accept";
    typed.content = AddressForm{
        (*raw.content)["street"].GetString(),
        (*raw.content)["city"].GetString(),
        (*raw.content)["zip_code"].GetString()
    };
}

if (typed.is_accepted() && typed.content) {
    auto& addr = *typed.content;
    // ...
}
```

`ElicitResultTyped<T>` members:

| Member | Type | Description |
|--------|------|-------------|
| `action` | `string` | `"accept"`, `"decline"`, or `"cancel"` (default) |
| `content` | `optional<T>` | Deserialized values (present on accept) |
| `is_accepted()` | `bool` | Returns `true` when `action == "accept"` |

Note: `ElicitResultTyped<T>` is not returned by any API — construct it manually from a raw `ElicitResult`.
