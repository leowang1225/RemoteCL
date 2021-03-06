// This file is part of RemoteCL.

// RemoteCL is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// RemoteCL is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.

// You should have received a copy of the GNU Lesser General Public License
// along with RemoteCL.  If not, see <https://www.gnu.org/licenses/>.

#include "objects.h"

#include <cstring>
#include "hints.h"
#include "connection.h"
#include "apiutil.h"
#include "packets/refcount.h"
#include "packets/program.h"
#include "packets/payload.h"

using namespace RemoteCL;
using namespace RemoteCL::Client;

SO_EXPORT CL_API_ENTRY cl_kernel CL_API_CALL
clCreateKernel(cl_program program, const char* kernel_name, cl_int* errcode_ret) CL_API_SUFFIX__VERSION_1_0
{
	if (program == nullptr) ReturnError(CL_INVALID_PROGRAM);
	if (kernel_name == nullptr) ReturnError(CL_INVALID_KERNEL_NAME);

	try {
		auto conn = gConnection.get();
		KernelName createKernel;
		createKernel.mID = GetID(program);
		createKernel.mString = kernel_name;
		conn->write(createKernel);
		conn->flush();
		IDType kernelID = conn->read<IDPacket>();
		if (errcode_ret) *errcode_ret = CL_SUCCESS;
		return conn.registerID<Kernel>(kernelID);
	} catch (const ErrorPacket& e) {
		ReturnError(e.mData);
	} catch (std::bad_alloc&) {
		ReturnError(CL_OUT_OF_HOST_MEMORY);
	} catch (...) {
		ReturnError(CL_DEVICE_NOT_AVAILABLE);
	}
}

SO_EXPORT CL_API_ENTRY cl_int CL_API_CALL
clCreateKernelsInProgram(cl_program program,
                         cl_uint num_kernels,
                         cl_kernel* kernels,
                         cl_uint* num_kernels_ret) CL_API_SUFFIX__VERSION_1_0
{
	if (program == nullptr) return CL_INVALID_PROGRAM;
	if (num_kernels != 0 && kernels == nullptr) return CL_INVALID_VALUE;
	if (num_kernels == 0 && kernels != nullptr) return CL_INVALID_VALUE;

	try {
		auto conn = gConnection.get();
		CreateKernels createKernels;
		createKernels.mProgramID = GetID(program);
		createKernels.mKernelCount = num_kernels;
		conn->write(createKernels);
		conn->flush();

		// Depending on operating mode, we either get the kernel count or the list.
		if (num_kernels == 0) {
			assert(kernels == nullptr && "Should have been validated.");
			auto count = conn->read<SimplePacket<PacketType::Payload, uint32_t>>();
			if (num_kernels_ret) {
				*num_kernels_ret = count.mData;
			}
			return CL_SUCCESS;
		}

		IDListPacket list = conn->read<IDListPacket>();
		assert(list.mIDs.size() <= num_kernels && "Remote API should have validated this.");
		for (IDType id : list.mIDs) {
			*kernels = conn.registerID<Kernel>(id);
			kernels++;
			if (num_kernels_ret) *num_kernels_ret += 1;
		}
		return CL_SUCCESS;
	} catch (const ErrorPacket& e) {
		return e.mData;
	} catch (std::bad_alloc&) {
		return CL_OUT_OF_HOST_MEMORY;
	} catch (...) {
		return CL_DEVICE_NOT_AVAILABLE;
	}
}

SO_EXPORT CL_API_ENTRY cl_int CL_API_CALL
clSetKernelArg(cl_kernel kernel, cl_uint arg_index,
               size_t arg_size, const void* arg_value) CL_API_SUFFIX__VERSION_1_0
{
	if (kernel == nullptr) return CL_INVALID_KERNEL;

	try {
		KernelArg arg;
		arg.mKernelID = GetID(kernel);
		arg.mArgIndex = arg_index;

		auto conn = gConnection.get();

		conn->write(arg);
		conn->flush();
		// Because we don't know if we need to translate the arg_value into an ID
		// that the server can understand, we need to wait for the server to tell us
		// what to do.
		SimplePacket<PacketType::Payload, char> what = conn->read<SimplePacket<PacketType::Payload, char>>();
		if (what.mData == 'I' && arg_size == sizeof(cl_mem)) {
			// Fetch the ID of the memory object.
			cl_mem obj = nullptr;
			// The following 2 lines will probably catastrophically fail if the host app did something stupid.
			std::memcpy(&obj, arg_value, sizeof(cl_mem));
			IDType id = GetID(obj);
			conn->write<IDPacket>(id);
		} else if (what.mData == 'S') {
			// This is a local-memory size packet.
			conn->write<SimplePacket<PacketType::Payload, uint32_t>>({static_cast<uint32_t>(arg_size)});
		} else {
			// Send the data verbatim.
			// This branch will also be taken if the sizeof() checks above are invalid,
			// in which case the server will send back some error.
			Payload<> payload;
			payload.mData.resize(arg_size);
			std::memcpy(payload.mData.data(), arg_value, arg_size);
			conn->write(payload);
		}
		conn->flush();
		conn->read<SuccessPacket>();
	} catch (const std::bad_alloc&) {
		return CL_OUT_OF_HOST_MEMORY;
	} catch (const ErrorPacket& e) {
		return e.mData;
	} catch (...) {
		return CL_DEVICE_NOT_AVAILABLE;
	}

	return CL_SUCCESS;
}

SO_EXPORT CL_API_ENTRY cl_int CL_API_CALL
clGetKernelWorkGroupInfo(cl_kernel kernel, cl_device_id device,
                         cl_kernel_work_group_info param_name, size_t param_value_size,
                         void* param_value, size_t*  param_value_size_ret) CL_API_SUFFIX__VERSION_1_0
{
	if (kernel == nullptr) return CL_INVALID_KERNEL;
	if (device == nullptr) return CL_INVALID_DEVICE;

	try {
		KernelWGInfo info;
		info.mKernelID = GetID(kernel);
		info.mDeviceID = GetID(device);
		info.mParam = param_name;

		auto conn = gConnection.get();
		conn->write(info).flush();

		switch (param_name) {
			case CL_KERNEL_COMPILE_WORK_GROUP_SIZE: {
				uint64_t sizes[3];
				conn->read(PayloadInto<uint8_t>(sizes));
				std::size_t* retVal = reinterpret_cast<std::size_t*>(param_value);
				if (param_value_size_ret) *param_value_size_ret = 3 * sizeof(std::size_t);
				if (retVal && param_value_size >= 3 * sizeof(std::size_t)) {
					retVal[0] = sizes[0];
					retVal[1] = sizes[1];
					retVal[2] = sizes[2];
				}
				break;
			}

			case CL_KERNEL_LOCAL_MEM_SIZE:
			case CL_KERNEL_PREFERRED_WORK_GROUP_SIZE_MULTIPLE:
			case CL_KERNEL_WORK_GROUP_SIZE: {
				std::size_t val = conn->read<SimplePacket<PacketType::Payload, uint64_t>>().mData;
				Store<std::size_t>(val, param_value, param_value_size, param_value_size_ret);
				break;
			}

			default: {
				Payload<> payload = conn->read<Payload<>>();
				if (param_value_size_ret) *param_value_size_ret = payload.mData.size();
				if (param_value && param_value_size >= payload.mData.size()) {
					std::memcpy(param_value, payload.mData.data(), payload.mData.size());
				}
				break;
			}
		}
		return CL_SUCCESS;
	} catch (const std::bad_alloc&) {
		return CL_OUT_OF_HOST_MEMORY;
	} catch (const ErrorPacket& e) {
		return e.mData;
	} catch (...) {
		return CL_DEVICE_NOT_AVAILABLE;
	}
}

SO_EXPORT CL_API_ENTRY cl_int CL_API_CALL
clGetKernelInfo(cl_kernel kernel, cl_kernel_info param_name, size_t param_value_size,
                void* param_value, size_t* param_value_size_ret) CL_API_SUFFIX__VERSION_1_0
{
	if (kernel == nullptr) return CL_INVALID_KERNEL;

	try {
		KernelInfo info;
		info.mID = GetID(kernel);
		info.mData = param_name;

		auto conn = gConnection.get();
		conn->write(info).flush();

		switch (param_name) {
			case CL_KERNEL_CONTEXT: {
				IDPacket ID = conn->read<IDPacket>();
				Store<cl_context>(conn.getOrInsertObject<Context>(ID),
				                  param_value, param_value_size, param_value_size_ret);
				break;
			}
			case CL_KERNEL_PROGRAM: {
				IDPacket ID = conn->read<IDPacket>();
				Store<cl_program>(conn.getOrInsertObject<Program>(ID),
				                  param_value, param_value_size, param_value_size_ret);
				break;
			}

			case CL_KERNEL_REFERENCE_COUNT:
			case CL_KERNEL_NUM_ARGS:  {
				uint32_t val = conn->read<SimplePacket<PacketType::Payload, uint32_t>>().mData;
				Store<cl_uint>(val, param_value, param_value_size, param_value_size_ret);
				break;
			}

			//case CL_KERNEL_FUNCTION_NAME:
			//case CL_KERNEL_ATTRIBUTES:
			default: {
				Payload<> payload = conn->read<Payload<>>();
				if (param_value_size_ret) *param_value_size_ret = payload.mData.size();
				if (param_value && param_value_size >= payload.mData.size()) {
					std::memcpy(param_value, payload.mData.data(), payload.mData.size());
				}
				break;
			}
		}
		return CL_SUCCESS;
	} catch (const std::bad_alloc&) {
		return CL_OUT_OF_HOST_MEMORY;
	} catch (const ErrorPacket& e) {
		return e.mData;
	} catch (...) {
		return CL_DEVICE_NOT_AVAILABLE;
	}
}

SO_EXPORT CL_API_ENTRY cl_int CL_API_CALL
clGetKernelArgInfo(cl_kernel kernel, cl_uint arg_indx, cl_kernel_arg_info param_name,
                   size_t param_value_size, void* param_value,
                   size_t* param_value_size_ret) CL_API_SUFFIX__VERSION_1_2
{
	if (kernel == nullptr) return CL_INVALID_KERNEL;

	try {
		KernelArgInfo info;
		info.mKernelID = GetID(kernel);
		info.mParam = param_name;
		info.mArgIndex = arg_indx;

		auto conn = gConnection.get();
		conn->write(info).flush();

		switch (param_name) {
			case CL_KERNEL_ARG_ACCESS_QUALIFIER: {
				uint32_t val = conn->read<SimplePacket<PacketType::Payload, uint32_t>>().mData;
				Store<cl_kernel_arg_address_qualifier>(val, param_value, param_value_size, param_value_size_ret);
				break;
			}
			case CL_KERNEL_ARG_TYPE_QUALIFIER: {
				uint32_t val = conn->read<SimplePacket<PacketType::Payload, uint32_t>>().mData;
				Store<cl_kernel_arg_type_qualifier>(val, param_value, param_value_size, param_value_size_ret);
				break;
			}
			case CL_KERNEL_ARG_ADDRESS_QUALIFIER: {
				uint32_t val = conn->read<SimplePacket<PacketType::Payload, uint32_t>>().mData;
				Store<cl_kernel_arg_access_qualifier>(val, param_value, param_value_size, param_value_size_ret);
				break;
			}

			//case CL_KERNEL_ARG_TYPE_NAME:
			//case CL_KERNEL_ARG_NAME:
			default: {
				Payload<> payload = conn->read<Payload<>>();
				if (param_value_size_ret) *param_value_size_ret = payload.mData.size();
				if (param_value && param_value_size >= payload.mData.size()) {
					std::memcpy(param_value, payload.mData.data(), payload.mData.size());
				}
				break;
			}
		}
		return CL_SUCCESS;
	} catch (const std::bad_alloc&) {
		return CL_OUT_OF_HOST_MEMORY;
	} catch (const ErrorPacket& e) {
		return e.mData;
	} catch (...) {
		return CL_DEVICE_NOT_AVAILABLE;
	}
}

SO_EXPORT CL_API_ENTRY cl_kernel CL_API_CALL
clCloneKernel(cl_kernel source_kernel, cl_int* errcode_ret) CL_API_SUFFIX__VERSION_2_1
{
	if (source_kernel == nullptr) ReturnError(CL_INVALID_KERNEL);

	try {
		auto conn = gConnection.get();
		conn->write<SimplePacket<PacketType::CloneKernel, IDType>>({GetID(source_kernel)}).flush();
		IDType kernelID = conn->read<IDPacket>();
		if (errcode_ret) *errcode_ret = CL_SUCCESS;
		return conn.registerID<Kernel>(kernelID);
	} catch (const std::bad_alloc&) {
		ReturnError(CL_OUT_OF_HOST_MEMORY);
	} catch (const ErrorPacket& e) {
		ReturnError(e.mData);
	} catch (...) {
		ReturnError(CL_DEVICE_NOT_AVAILABLE);
	}
}

SO_EXPORT CL_API_ENTRY cl_int CL_API_CALL
clRetainKernel(cl_kernel kernel) CL_API_SUFFIX__VERSION_1_0
{
	if (kernel == nullptr) return CL_INVALID_KERNEL;

	try {
		auto conn = gConnection.get();
		conn->write<Retain>({'K', GetID(kernel)});
		conn->flush();
		conn->read<SuccessPacket>();
	} catch (const ErrorPacket& e) {
		return e.mData;
	} catch (...) {
		return CL_DEVICE_NOT_AVAILABLE;
	}

	return CL_SUCCESS;
}

SO_EXPORT CL_API_ENTRY cl_int CL_API_CALL
clReleaseKernel(cl_kernel kernel) CL_API_SUFFIX__VERSION_1_0
{
	if (kernel == nullptr) return CL_INVALID_KERNEL;

	try {
		auto conn = gConnection.get();
		conn->write<Release>({'K', GetID(kernel)});
		conn->flush();
		conn->read<SuccessPacket>();
	} catch (const ErrorPacket& e) {
		return e.mData;
	} catch (...) {
		return CL_DEVICE_NOT_AVAILABLE;
	}

	return CL_SUCCESS;
}
