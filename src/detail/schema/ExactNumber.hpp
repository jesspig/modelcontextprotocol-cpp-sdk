#pragma once
#include <mcp/JsonValue.hpp>
#include <mcp/McpError.hpp>

#include <charconv>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mcp { namespace detail { namespace schema {

[[noreturn]] inline void NumberFail(const char* classifier, const std::string& message) {
    throw McpError(McpErrorCode::InvalidParams, std::string(classifier) + ": " + message);
}

class BigUInt {
public:
    static constexpr uint32_t kBase = 1000000000u;

    BigUInt() = default;

    explicit BigUInt(uint64_t value) {
        while (value > 0) {
            limbs_.push_back(static_cast<uint32_t>(value % kBase));
            value /= kBase;
        }
    }

    static BigUInt FromDigits(std::string_view digits) {
        BigUInt result;
        for (char c : digits) {
            if (c < '0' || c > '9') NumberFail("SL.NumberRange", "invalid decimal digit");
            result = result.MultiplySmall(10);
            result.AddSmall(static_cast<uint32_t>(c - '0'));
        }
        return result;
    }

    static BigUInt Pow10(size_t exponent) {
        if (exponent > 400) NumberFail("SL.NumberRange", "decimal exponent beyond budget");
        BigUInt result(1);
        for (size_t i = 0; i < exponent; ++i) result = result.MultiplySmall(10);
        return result;
    }

    [[nodiscard]] bool IsZero() const { return limbs_.empty(); }

    void Trim() {
        while (!limbs_.empty() && limbs_.back() == 0) limbs_.pop_back();
    }

    void AddSmall(uint32_t value) {
        size_t i = 0;
        uint64_t carry = value;
        while (carry > 0) {
            if (i == limbs_.size()) limbs_.push_back(0);
            uint64_t next = limbs_[i] + carry;
            limbs_[i] = static_cast<uint32_t>(next % kBase);
            carry = next / kBase;
            ++i;
        }
    }

    [[nodiscard]] BigUInt MultiplySmall(uint32_t value) const {
        if (value == 0 || limbs_.empty()) return BigUInt();
        BigUInt result;
        result.limbs_.resize(limbs_.size());
        uint64_t carry = 0;
        for (size_t i = 0; i < limbs_.size(); ++i) {
            uint64_t next = static_cast<uint64_t>(limbs_[i]) * value + carry;
            result.limbs_[i] = static_cast<uint32_t>(next % kBase);
            carry = next / kBase;
        }
        while (carry > 0) {
            result.limbs_.push_back(static_cast<uint32_t>(carry % kBase));
            carry /= kBase;
        }
        return result;
    }

    [[nodiscard]] BigUInt Add(const BigUInt& other) const {
        BigUInt result;
        size_t n = limbs_.size() > other.limbs_.size() ? limbs_.size() : other.limbs_.size();
        result.limbs_.resize(n);
        uint64_t carry = 0;
        for (size_t i = 0; i < n; ++i) {
            uint64_t next = carry;
            if (i < limbs_.size()) next += limbs_[i];
            if (i < other.limbs_.size()) next += other.limbs_[i];
            result.limbs_[i] = static_cast<uint32_t>(next % kBase);
            carry = next / kBase;
        }
        if (carry > 0) result.limbs_.push_back(static_cast<uint32_t>(carry));
        result.Trim();
        return result;
    }

    [[nodiscard]] BigUInt Sub(const BigUInt& other) const {
        if (Compare(other) < 0) NumberFail("SL.NumberRange", "negative subtraction");
        BigUInt result;
        result.limbs_.resize(limbs_.size());
        int64_t borrow = 0;
        for (size_t i = 0; i < limbs_.size(); ++i) {
            int64_t next = static_cast<int64_t>(limbs_[i]) - borrow;
            if (i < other.limbs_.size()) next -= static_cast<int64_t>(other.limbs_[i]);
            if (next < 0) {
                next += kBase;
                borrow = 1;
            } else {
                borrow = 0;
            }
            result.limbs_[i] = static_cast<uint32_t>(next);
        }
        result.Trim();
        return result;
    }

    [[nodiscard]] BigUInt Multiply(const BigUInt& other) const {
        if (limbs_.empty() || other.limbs_.empty()) return BigUInt();
        BigUInt result;
        result.limbs_.assign(limbs_.size() + other.limbs_.size() + 1, 0);
        for (size_t i = 0; i < limbs_.size(); ++i) {
            uint64_t carry = 0;
            for (size_t j = 0; j < other.limbs_.size(); ++j) {
                uint64_t next = result.limbs_[i + j]
                    + static_cast<uint64_t>(limbs_[i]) * other.limbs_[j] + carry;
                result.limbs_[i + j] = static_cast<uint32_t>(next % kBase);
                carry = next / kBase;
            }
            size_t k = i + other.limbs_.size();
            while (carry > 0) {
                uint64_t next = result.limbs_[k] + carry;
                result.limbs_[k] = static_cast<uint32_t>(next % kBase);
                carry = next / kBase;
                ++k;
            }
        }
        result.Trim();
        return result;
    }

    void DivMod(const BigUInt& divisor, BigUInt& quotient, BigUInt& remainder) const {
        if (divisor.IsZero()) NumberFail("SL.NumberRange", "division by zero");
        quotient.limbs_.assign(limbs_.size(), 0);
        remainder = BigUInt();
        for (size_t i = limbs_.size(); i-- > 0;) {
            remainder = remainder.MultiplySmall(kBase);
            remainder.AddSmall(limbs_[i]);
            uint32_t low = 0;
            uint32_t high = kBase - 1;
            uint32_t digit = 0;
            while (low <= high) {
                uint32_t mid = low + (high - low) / 2;
                BigUInt trial = divisor.MultiplySmall(mid);
                if (trial.Compare(remainder) <= 0) {
                    digit = mid;
                    if (mid == kBase - 1) break;
                    low = mid + 1;
                } else {
                    if (mid == 0) break;
                    high = mid - 1;
                }
            }
            quotient.limbs_[i] = digit;
            if (digit > 0) remainder = remainder.Sub(divisor.MultiplySmall(digit));
        }
        quotient.Trim();
        remainder.Trim();
    }

    [[nodiscard]] bool DivisibleBy(const BigUInt& divisor) const {
        BigUInt quotient;
        BigUInt remainder;
        DivMod(divisor, quotient, remainder);
        return remainder.IsZero();
    }

    [[nodiscard]] int Compare(const BigUInt& other) const {
        if (limbs_.size() != other.limbs_.size()) {
            return limbs_.size() < other.limbs_.size() ? -1 : 1;
        }
        for (size_t i = limbs_.size(); i-- > 0;) {
            if (limbs_[i] != other.limbs_[i]) return limbs_[i] < other.limbs_[i] ? -1 : 1;
        }
        return 0;
    }

    [[nodiscard]] uint64_t ToUint64() const {
        uint64_t value = 0;
        for (uint32_t limb : limbs_) value = value * kBase + limb;
        return value;
    }

    [[nodiscard]] std::string ToString() const {
        if (limbs_.empty()) return "0";
        std::string out = std::to_string(limbs_.back());
        for (size_t i = limbs_.size() - 1; i-- > 0;) {
            std::string part = std::to_string(limbs_[i]);
            out.append(9 - part.size(), '0');
            out += part;
        }
        return out;
    }

private:
    std::vector<uint32_t> limbs_;
};

class ExactNumber {
public:
    ExactNumber() : sign_(0), numerator_(), denominator_(1) {}

    static ExactNumber FromJson(const JsonValue& value) {
        ExactNumber result;
        if (value.IsInt()) {
            int64_t raw = value.GetInt();
            if (raw == 0) return result;
            result.sign_ = raw < 0 ? -1 : 1;
            uint64_t magnitude = raw < 0
                ? static_cast<uint64_t>(-(raw + 1)) + 1u
                : static_cast<uint64_t>(raw);
            result.numerator_ = BigUInt(magnitude);
            result.denominator_ = BigUInt(1);
            return result;
        }
        if (!value.IsDouble()) NumberFail("SL.NumberRange", "expected numeric value");
        double number = value.GetDouble();
        if (!std::isfinite(number)) NumberFail("SL.NumberRange", "non-finite number");
        char buffer[128];
        auto converted = std::to_chars(buffer, buffer + sizeof(buffer), number,
                                        std::chars_format::general);
        if (converted.ec != std::errc()) NumberFail("SL.NumberRange", "number serialization failed");
        std::string_view text(buffer, static_cast<size_t>(converted.ptr - buffer));
        size_t pos = 0;
        int sign = 1;
        if (pos < text.size() && (text[pos] == '-' || text[pos] == '+')) {
            if (text[pos] == '-') sign = -1;
            ++pos;
        }
        std::string digits;
        while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
            digits.push_back(text[pos]);
            ++pos;
        }
        if (digits.empty()) NumberFail("SL.NumberRange", "number without digits");
        int power = 0;
        if (pos < text.size() && text[pos] == '.') {
            ++pos;
            while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
                digits.push_back(text[pos]);
                --power;
                ++pos;
            }
        }
        if (pos < text.size() && (text[pos] == 'e' || text[pos] == 'E')) {
            ++pos;
            int exponent_sign = 1;
            if (pos < text.size() && (text[pos] == '-' || text[pos] == '+')) {
                if (text[pos] == '-') exponent_sign = -1;
                ++pos;
            }
            int exponent = 0;
            while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
                exponent = exponent * 10 + (text[pos] - '0');
                if (exponent > 400) NumberFail("SL.NumberRange", "decimal exponent beyond budget");
                ++pos;
            }
            power += exponent_sign * exponent;
        }
        if (pos != text.size()) NumberFail("SL.NumberRange", "invalid numeric representation");
        if (digits.size() > 64) NumberFail("SL.NumberRange", "numeric precision beyond budget");
        if (power < -400 || power > 400) NumberFail("SL.NumberRange", "decimal exponent beyond budget");
        bool all_zero = true;
        for (char c : digits) {
            if (c != '0') {
                all_zero = false;
                break;
            }
        }
        if (all_zero) return result;
        result.sign_ = sign;
        result.numerator_ = BigUInt::FromDigits(digits);
        result.denominator_ = power >= 0 ? BigUInt(1) : BigUInt::Pow10(static_cast<size_t>(-power));
        if (power > 0) result.numerator_ = result.numerator_.Multiply(BigUInt::Pow10(static_cast<size_t>(power)));
        result.numerator_.Trim();
        result.denominator_.Trim();
        return result;
    }

    [[nodiscard]] int Compare(const ExactNumber& other) const {
        if (sign_ != other.sign_) return sign_ < other.sign_ ? -1 : 1;
        if (sign_ == 0) return 0;
        BigUInt left = numerator_.Multiply(other.denominator_);
        BigUInt right = other.numerator_.Multiply(denominator_);
        int magnitude = left.Compare(right);
        return sign_ < 0 ? -magnitude : magnitude;
    }

    [[nodiscard]] bool IsZero() const { return sign_ == 0; }

    [[nodiscard]] bool IsPositive() const { return sign_ > 0; }

    [[nodiscard]] bool IsInteger() const { return numerator_.DivisibleBy(denominator_); }

    [[nodiscard]] bool IsMultipleOf(const ExactNumber& divisor) const {
        if (divisor.IsZero()) return false;
        BigUInt left = numerator_.Multiply(divisor.denominator_);
        BigUInt right = denominator_.Multiply(divisor.numerator_);
        return left.DivisibleBy(right);
    }

    [[nodiscard]] uint64_t ToBoundedInteger(uint64_t limit) const {
        if (sign_ < 0 || !IsInteger()) NumberFail("SL.NumberRange", "expected non-negative integer");
        BigUInt quotient;
        BigUInt remainder;
        numerator_.DivMod(denominator_, quotient, remainder);
        if (quotient.Compare(BigUInt(limit)) > 0) NumberFail("SL.NumberRange", "integer beyond budget");
        return quotient.ToUint64();
    }

    [[nodiscard]] std::string ToString() const {
        if (sign_ == 0) return "0";
        return std::string(sign_ < 0 ? "-" : "") + numerator_.ToString() + "/" + denominator_.ToString();
    }

private:
    int sign_;
    BigUInt numerator_;
    BigUInt denominator_;
};

}}}
