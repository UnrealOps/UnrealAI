// Copyright EngineWorks. All Rights Reserved.
#include "Serialization/UnrealAIDigest.h"
#include <array>
namespace
{
constexpr std::array<uint32, 64> Sha256RoundConstants = {
	0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
	0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
	0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
	0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
	0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
	0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
	0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
	0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

uint32 RotateRight(const uint32 Value, const uint32 Count)
{
	return (Value >> Count) | (Value << (32u - Count));
}

void TransformSha256Block(const uint8 *Block, std::array<uint32, 8> &State)
{
	std::array<uint32, 64> Schedule{};
	for (int32 WordIndex = 0; WordIndex < 16; ++WordIndex)
	{
		const int32 Offset = WordIndex * 4;
		Schedule[WordIndex] = (static_cast<uint32>(Block[Offset]) << 24u) |
							  (static_cast<uint32>(Block[Offset + 1]) << 16u) |
							  (static_cast<uint32>(Block[Offset + 2]) << 8u) | static_cast<uint32>(Block[Offset + 3]);
	}
	for (int32 WordIndex = 16; WordIndex < 64; ++WordIndex)
	{
		const uint32 Sigma0 = RotateRight(Schedule[WordIndex - 15], 7u) ^ RotateRight(Schedule[WordIndex - 15], 18u) ^
							  (Schedule[WordIndex - 15] >> 3u);
		const uint32 Sigma1 = RotateRight(Schedule[WordIndex - 2], 17u) ^ RotateRight(Schedule[WordIndex - 2], 19u) ^
							  (Schedule[WordIndex - 2] >> 10u);
		Schedule[WordIndex] = Schedule[WordIndex - 16] + Sigma0 + Schedule[WordIndex - 7] + Sigma1;
	}

	uint32 A = State[0];
	uint32 B = State[1];
	uint32 C = State[2];
	uint32 D = State[3];
	uint32 E = State[4];
	uint32 F = State[5];
	uint32 G = State[6];
	uint32 H = State[7];
	for (int32 Round = 0; Round < 64; ++Round)
	{
		const uint32 BigSigma1 = RotateRight(E, 6u) ^ RotateRight(E, 11u) ^ RotateRight(E, 25u);
		const uint32 Choice = (E & F) ^ ((~E) & G);
		const uint32 Temp1 = H + BigSigma1 + Choice + Sha256RoundConstants[Round] + Schedule[Round];
		const uint32 BigSigma0 = RotateRight(A, 2u) ^ RotateRight(A, 13u) ^ RotateRight(A, 22u);
		const uint32 Majority = (A & B) ^ (A & C) ^ (B & C);
		const uint32 Temp2 = BigSigma0 + Majority;
		H = G;
		G = F;
		F = E;
		E = D + Temp1;
		D = C;
		C = B;
		B = A;
		A = Temp1 + Temp2;
	}

	State[0] += A;
	State[1] += B;
	State[2] += C;
	State[3] += D;
	State[4] += E;
	State[5] += F;
	State[6] += G;
	State[7] += H;
}

FString Sha256LowerHex(const uint8 *Data, const uint64 Size)
{
	std::array<uint32, 8> State = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
								   0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
	const uint64 FullBlockBytes = Size - (Size % 64u);
	for (uint64 Offset = 0; Offset < FullBlockBytes; Offset += 64u)
	{
		TransformSha256Block(Data + static_cast<SIZE_T>(Offset), State);
	}

	std::array<uint8, 128> FinalBlocks{};
	const uint64 RemainingBytes = Size - FullBlockBytes;
	if (RemainingBytes > 0u)
	{
		FMemory::Memcpy(FinalBlocks.data(), Data + static_cast<SIZE_T>(FullBlockBytes),
						static_cast<SIZE_T>(RemainingBytes));
	}
	FinalBlocks[static_cast<SIZE_T>(RemainingBytes)] = 0x80u;
	const SIZE_T FinalSize = RemainingBytes < 56u ? 64u : 128u;
	const uint64 BitLength = Size * 8u;
	for (SIZE_T ByteIndex = 0; ByteIndex < 8u; ++ByteIndex)
	{
		FinalBlocks[FinalSize - 1u - ByteIndex] = static_cast<uint8>(BitLength >> (ByteIndex * 8u));
	}
	for (SIZE_T Offset = 0; Offset < FinalSize; Offset += 64u)
	{
		TransformSha256Block(FinalBlocks.data() + Offset, State);
	}

	static constexpr TCHAR HexDigits[] = TEXT("0123456789abcdef");
	FString Result;
	Result.Reserve(64);
	for (const uint32 Word : State)
	{
		for (int32 Shift = 24; Shift >= 0; Shift -= 8)
		{
			const uint8 Byte = static_cast<uint8>(Word >> static_cast<uint32>(Shift));
			Result.AppendChar(HexDigits[Byte >> 4u]);
			Result.AppendChar(HexDigits[Byte & 0x0fu]);
		}
	}
	return Result;
}

} // namespace
bool UE::UnrealAI::Digest::Sha256(TConstArrayView<uint8> Bytes, FString &OutLowerHex)
{
	OutLowerHex.Reset();
	if (Bytes.Num() > 64 * 1024 * 1024)
	{
		return false;
	}
	OutLowerHex = Sha256LowerHex(Bytes.GetData(), Bytes.Num());
	return true;
}
