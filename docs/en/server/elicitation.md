# Elicitation

Elicitation allows the server to request additional input from the user during tool execution. It replaces the deprecated Sampling mechanism (SEP-2577).

## Form Mode

Requests structured user input via a JSON Schema form:

```cpp
ElicitRequestParams params;
params.message = "Please provide your shipping address";
params.requested_schema = R"({
    "type": "object",
    "properties": {
        "street": {"type": "string"},
        "city": {"type": "string"},
        "zip": {"type": "string"}
    },
    "required": ["street", "city", "zip"]
})"_json;

auto result = server->Elicit(params);
if (result.get().values) {
    auto street = (*result.get().values)["street"];
}
```

## Generic Typed Form

Use the `Elicit<T>` template for type-safe elicitation:

```cpp
struct AddressForm {
    std::string street;
    std::string city;
    std::string zip_code;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(AddressForm, street, city, zip_code)

auto future = server->Elicit<AddressForm>("Please provide your shipping address");
auto result = future.get();
if (result.is_accepted() && result.content) {
    auto& addr = *result.content;
    std::cout << addr.street << ", " << addr.city << "\n";
}
```

## Elicitation Result

The result has a three-value action:

| Action | Meaning |
|--------|---------|
| `accept` | User submitted the form |
| `decline` | User explicitly declined |
| `cancel` | User dismissed without action (default) |
