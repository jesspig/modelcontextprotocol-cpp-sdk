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
if (result.values) {
    auto street = (*result.values)["street"];
}
```

## Typed Helpers

`ElicitResultTyped<T>` is a user-side convenience wrapper for deserializing elicitation results into a typed structure:

```cpp
struct AddressForm {
    std::string street;
    std::string city;
    std::string zip_code;
};

ElicitResult raw = future.get();
ElicitResultTyped<AddressForm> typed;
if (raw.values) {
    typed.action = "accept";
    typed.content = AddressForm{
        (*raw.values)["street"].GetString(),
        (*raw.values)["city"].GetString(),
        (*raw.values)["zip_code"].GetString()
    };
}

if (typed.is_accepted() && typed.content) {
    auto& addr = *typed.content;
    // ...
}
```

## Elicitation Result

`ElicitResult` extends `Result` with the following fields:

| Field | Type | Description |
|-------|------|-------------|
| `values` | `std::optional<JsonValue>` | Submitted form data (present on accept) |
| `result_type` | `ResultType` | `Complete` (accepted) or `InputRequired` (declined/pending) |

`ElicitResultTyped<T>` provides a user-side action model:

| Member | Type | Description |
|--------|------|-------------|
| `action` | `std::string` | `"accept"`, `"decline"`, or `"cancel"` (default) |
| `content` | `std::optional<T>` | Deserialized values (present on accept) |
