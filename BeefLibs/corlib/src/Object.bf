using System;
using System.Reflection;
using System.Collections;
using System.Diagnostics;

namespace System
{
    class Object : IHashable
    {
#if BF_ENABLE_OBJECT_DEBUG_FLAGS
        int mClassVData;
        int mDbgAllocInfo;
#else        
        ClassVData* mClassVData;
#endif
    
        public virtual ~this()
        {
#if BF_ENABLE_OBJECT_DEBUG_FLAGS
			mClassVData = ((mClassVData & ~0x08) | 0x80);
#endif
        }

#if BF_ENABLE_OBJECT_DEBUG_FLAGS
		[NoShow]
		int32 GetFlags()
		{
			return (int32)mClassVData & 0xFF;
		}

        [DisableObjectAccessChecks, NoShow]
        public bool IsDeleted()
        {
            return (int32)mClassVData & 0x80 != 0;
        }
#else
        [SkipCall]
        public bool IsDeleted()
        {
            return false;
        }
#endif
		extern Type Comptime_GetType();

        public Type GetType()
        {
			if (Compiler.IsComptime)
				return Comptime_GetType();

            Type type;
#if BF_ENABLE_OBJECT_DEBUG_FLAGS
            ClassVData* maskedVData = (ClassVData*)(void*)(mClassVData & ~(int)0xFF);
            type = maskedVData.mType;
#else
            type = mClassVData.mType;
#endif
            if ((type.[Friend]mTypeFlags & TypeFlags.Boxed) != 0)
            {
                //int32 underlyingType = (int32)((TypeInstance)type).mUnderlyingType;
                type = Type.[Friend]GetType(((TypeInstance)type).[Friend]mUnderlyingType);
            }
            return type;
        }

		[NoShow]
        Type RawGetType()
        {
			if (Compiler.IsComptime)
				return Comptime_GetType();

            Type type;
#if BF_ENABLE_OBJECT_DEBUG_FLAGS
            ClassVData* maskedVData = (ClassVData*)(void*)(mClassVData & ~(int)0xFF);
            type = maskedVData.mType;
#else            
            type = mClassVData.mType;
#endif            
            return type;
        }

#if BF_DYNAMIC_CAST_CHECK || BF_ENABLE_REALTIME_LEAK_CHECK
		[NoShow]
		public virtual Object DynamicCastToTypeId(int32 typeId)
		{
		    if (typeId == (int32)RawGetType().[Friend]mTypeId)
		        return this;
		    return null;
		}

		[NoShow]
		public virtual Object DynamicCastToInterface(int32 typeId)
		{
		    return null;
		}
#endif

        int IHashable.GetHashCode()
        {
            return (int)Internal.UnsafeCastToPtr(this);
        }
        
        public virtual void ToString(String strBuffer)
        {
			let t = RawGetType();
			if (t.IsBoxedStructPtr)
			{
				let ti = (TypeInstance)t;
				let innerPtr = *(void**)((uint8*)Internal.UnsafeCastToPtr(this) + ti.[Friend]mMemberDataOffset);
				strBuffer.Append("(");
				ti.UnderlyingType.GetFullName(strBuffer);
				strBuffer.AppendF("*)0x{0:A}", (uint)(void*)innerPtr);
				return;
			}
            t.GetFullName(strBuffer);
			strBuffer.Append("@0x");

			((int)Internal.UnsafeCastToPtr(this)).ToString(strBuffer, "A", null);
        }

		private static void ToString(Object obj, String strBuffer)
		{
			if (obj == null)
				strBuffer.Append("null");
			else
				obj.ToString(strBuffer);
		}
                
        [SkipCall, NoShow]
    	protected virtual void GCMarkMembers()
        {
            //PrintF("Object.GCMarkMembers %08X\n", this);
		}
    }
}

